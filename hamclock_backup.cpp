#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Multi_Browser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/mount.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>

#include "cfg_info_format.h"

#ifndef HAMCLOCK_VERSION
#define HAMCLOCK_VERSION ""
#endif

namespace fs = std::filesystem;

static std::string trim(const std::string &s)
{
    const char *ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return "";
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static std::string basenameToDisplayName(const fs::path &p)
{
    std::string stem = p.filename().string();
    const std::string suffix = ".eeprom";
    if (stem.size() >= suffix.size() && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
        stem.erase(stem.size() - suffix.size());
    std::replace(stem.begin(), stem.end(), '_', ' ');
    return stem;
}

static std::string fmtTime(std::time_t t)
{
    if (t <= 0)
        return "";
    char buf[64];
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv);
    return buf;
}

static std::string fileMTime(const fs::path &p)
{
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec)
        return "";
    // C++17 has no portable clock_cast. This approximation is widely used.
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return fmtTime(std::chrono::system_clock::to_time_t(sctp));
}

class Crc32 {
public:
    Crc32() { reset(); }
    void reset() { crc_ = 0xFFFFFFFFu; }
    void update(const uint8_t *data, size_t len)
    {
        initTable();
        for (size_t i = 0; i < len; i++)
            crc_ = table_[(crc_ ^ data[i]) & 0xFFu] ^ (crc_ >> 8);
    }
    uint32_t final() const { return crc_ ^ 0xFFFFFFFFu; }

    static std::optional<uint32_t> file(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        Crc32 c;
        char buf[32768];
        while (in) {
            in.read(buf, sizeof(buf));
            const std::streamsize n = in.gcount();
            if (n > 0)
                c.update(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(n));
        }
        if (in.bad())
            return std::nullopt;
        return c.final();
    }

    static std::string hex(uint32_t v)
    {
        std::ostringstream os;
        os << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << v;
        return os.str();
    }

private:
    uint32_t crc_;
    static uint32_t table_[256];
    static bool table_ready_;

    static void initTable()
    {
        if (table_ready_)
            return;
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table_[i] = c;
        }
        table_ready_ = true;
    }
};

uint32_t Crc32::table_[256];
bool Crc32::table_ready_ = false;

struct SidecarInfo {
    bool present = false;
    bool valid = false;
    std::map<std::string, std::string> kv;

    std::string get(const std::string &key, const std::string &fallback = "") const
    {
        auto it = kv.find(key);
        return it == kv.end() || it->second.empty() ? fallback : it->second;
    }
};

static SidecarInfo parseSidecar(const fs::path &path)
{
    SidecarInfo info;
    std::ifstream in(path);
    if (!in)
        return info;
    info.present = true;

    bool in_section = false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            in_section = trim(line.substr(1, line.size() - 2)) == HC_INFO_SECTION;
            continue;
        }
        if (!in_section)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        info.kv[key] = val;
    }

    info.valid = info.get(HC_INFO_KEY_FORMAT_VER) == std::to_string(HC_INFO_FORMAT_VERSION);
    return info;
}

struct ConfigItem {
    fs::path eeprom_path;
    fs::path sidecar_path;
    SidecarInfo info;
    std::string name;
    std::string created;
    std::string from;
    std::string version;
    std::string size_text;
    bool has_metadata = false;
};

struct UsbMount {
    std::string path;
    std::string fs_type;
    std::string free_text;
};

static uintmax_t fileSizeOrZero(const fs::path &p)
{
    std::error_code ec;
    uintmax_t s = fs::file_size(p, ec);
    return ec ? 0 : s;
}

static std::string humanBytes(uintmax_t n)
{
    std::ostringstream os;
    if (n < 1024) {
        os << n << " B";
    } else if (n < 1024 * 1024) {
        os << std::fixed << std::setprecision(1) << (double)n / 1024.0 << " KB";
    } else {
        os << std::fixed << std::setprecision(1) << (double)n / (1024.0 * 1024.0) << " MB";
    }
    return os.str();
}

static std::string freeSpaceText(const fs::path &p)
{
    std::error_code ec;
    auto sp = fs::space(p, ec);
    if (ec)
        return "";
    return humanBytes(sp.available) + " free";
}

static ConfigItem makeConfigItem(const fs::path &eeprom_path)
{
    ConfigItem item;
    item.eeprom_path = eeprom_path;
    item.sidecar_path = fs::path(eeprom_path.string() + HC_INFO_SUFFIX);
    item.info = parseSidecar(item.sidecar_path);
    item.has_metadata = item.info.present && item.info.valid;
    item.name = item.info.get(HC_INFO_KEY_CONFIG_NAME, basenameToDisplayName(eeprom_path));
    item.created = item.info.get(HC_INFO_KEY_CREATED, fileMTime(eeprom_path));
    item.from = item.info.get(HC_INFO_KEY_HOSTNAME, item.has_metadata ? "" : "(no metadata)");
    item.version = item.info.get(HC_INFO_KEY_HC_VERSION, "");
    const std::string meta_size = item.info.get(HC_INFO_KEY_EEPROM_SIZE);
    item.size_text = meta_size.empty() ? humanBytes(fileSizeOrZero(eeprom_path)) : meta_size + " B";
    return item;
}

static std::vector<ConfigItem> scanConfigs(const fs::path &dir)
{
    std::vector<ConfigItem> out;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        return out;
    for (const auto &ent : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!ent.is_regular_file(ec))
            continue;
        const fs::path p = ent.path();
        if (p.extension() == ".eeprom")
            out.push_back(makeConfigItem(p));
    }
    std::sort(out.begin(), out.end(), [](const ConfigItem &a, const ConfigItem &b) {
        return a.name < b.name;
    });
    return out;
}

static bool isWslDriveMount(const std::string &path)
{
    // WSL exposes Windows drive letters as /mnt/<letter> via drvfs.
    // /mnt/c is usually the Windows system drive, so do not auto-treat it as USB.
    if (path.size() != 6 || path.rfind("/mnt/", 0) != 0)
        return false;
    char drive = path[5];
    return drive >= 'd' && drive <= 'z';
}


static bool isSystemFatMountPath(const std::string &path)
{
    // Raspberry Pi OS mounts its boot FAT partition at /boot/firmware. That is
    // not a removable backup target and must not count as a USB drive.
    return path == "/boot"
        || path.rfind("/boot/", 0) == 0
        || path == "/efi"
        || path.rfind("/efi/", 0) == 0;
}

static bool isAllowedFs(const std::string &fs, const std::string &path, bool explicit_override)
{
    if (fs == "vfat" || fs == "msdos" || fs == "exfat" || fs == "msdosfs")
        return true;

    // WSL can expose Windows drives either as drvfs or as 9p with drvfs details
    // in the mount options, for example:
    //   D: on /mnt/d type 9p (...,aname=drvfs;path=D:,...)
    // Accept this only for an explicit override, or for /mnt/d..z. This avoids
    // accidentally treating the Windows system drive /mnt/c as a USB target.
    if ((fs == "drvfs" || fs == "9p") && (explicit_override || isWslDriveMount(path)))
        return true;

    return false;
}

static bool looksLikeUsbMountPath(const std::string &path)
{
    if (isSystemFatMountPath(path))
        return false;
#ifdef __APPLE__
    return path.rfind("/Volumes/", 0) == 0 && path != "/Volumes/Macintosh HD";
#else
    return path.rfind("/media/", 0) == 0 || path.rfind("/run/media/", 0) == 0 || path.rfind("/mnt/", 0) == 0;
#endif
}

static std::vector<UsbMount> findUsbMounts()
{
    std::vector<UsbMount> mounts;

    // Optional escape hatch, especially useful on WSL where removable Windows drives
    // are surfaced as drvfs rather than vfat/exfat. Example:
    //   HAMCLOCK_BACKUP_USB=/mnt/e ./hamclock-backup
    const char *override_path = getenv("HAMCLOCK_BACKUP_USB");
    if (override_path && *override_path) {
        std::string path = override_path;
        std::string type = "override";
#ifndef __APPLE__
        std::ifstream in("/proc/mounts");
        std::string dev, mpath, mtype, rest;
        while (in >> dev >> mpath >> mtype >> rest) {
            std::getline(in, rest);
            if (mpath == path) {
                type = mtype;
                break;
            }
        }
#endif
        if (access(path.c_str(), R_OK | W_OK) == 0 && (type == "override" || isAllowedFs(type, path, true)))
            mounts.push_back({path, type, freeSpaceText(path)});
        return mounts;
    }

#ifdef __APPLE__
    struct statfs *mntbuf = nullptr;
    int n = getmntinfo(&mntbuf, MNT_NOWAIT);
    for (int i = 0; i < n; i++) {
        std::string path = mntbuf[i].f_mntonname;
        std::string type = mntbuf[i].f_fstypename;
        if (looksLikeUsbMountPath(path) && isAllowedFs(type, path, false) && access(path.c_str(), R_OK | W_OK) == 0)
            mounts.push_back({path, type, freeSpaceText(path)});
    }
#else
    std::ifstream in("/proc/mounts");
    std::string dev, path, type, rest;
    while (in >> dev >> path >> type >> rest) {
        std::getline(in, rest);
        if (looksLikeUsbMountPath(path) && isAllowedFs(type, path, false) && access(path.c_str(), R_OK | W_OK) == 0)
            mounts.push_back({path, type, freeSpaceText(path)});
    }
#endif
    std::sort(mounts.begin(), mounts.end(), [](const UsbMount &a, const UsbMount &b) {
        return a.path < b.path;
    });
    return mounts;
}

static bool isHamClockRunning(const fs::path &live_eeprom)
{
    int fd = open(live_eeprom.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    int rc = flock(fd, LOCK_EX | LOCK_NB);
    if (rc == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }
    const bool locked = errno == EWOULDBLOCK || errno == EAGAIN;
    close(fd);
    return locked;
}

static bool copyBytesOnly(const fs::path &src, const fs::path &dst, std::string &err)
{
    // Avoid std::filesystem::copy_file here. On WSL/Windows-mounted drives it can
    // fail with EPERM while trying to preserve POSIX-ish metadata. We only need
    // the bytes copied; CRC verification below proves the payload arrived intact.
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        err = "Could not open source: " + src.string();
        return false;
    }

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "Could not open destination: " + dst.string();
        return false;
    }

    out << in.rdbuf();
    if (!in.eof() && in.fail()) {
        err = "Read failed while copying: " + src.string();
        return false;
    }
    out.flush();
    if (!out) {
        err = "Write failed while copying: " + dst.string();
        return false;
    }

    return true;
}

static bool copyFileWithCrcVerify(const fs::path &src, const fs::path &dst, std::string &err)
{
    auto src_crc = Crc32::file(src);
    if (!src_crc) {
        err = "Could not read source for CRC: " + src.string();
        return false;
    }

    if (!copyBytesOnly(src, dst, err)) {
        err = "Copy failed: " + err;
        return false;
    }

    auto dst_crc = Crc32::file(dst);
    if (!dst_crc) {
        err = "Could not read destination for CRC: " + dst.string();
        return false;
    }
    if (*src_crc != *dst_crc) {
        err = "CRC mismatch: " + Crc32::hex(*src_crc) + " != " + Crc32::hex(*dst_crc);
        return false;
    }
    return true;
}

static bool copySidecarIfPresent(const fs::path &src_sidecar, const fs::path &dst_sidecar, std::string &err)
{
    std::error_code ec;
    if (!fs::exists(src_sidecar, ec))
        return true;
    if (!copyBytesOnly(src_sidecar, dst_sidecar, err)) {
        err = "Sidecar copy failed: " + err;
        return false;
    }
    return true;
}


class UsbChooserWindow : public Fl_Window {
public:
    UsbChooserWindow(const std::vector<UsbMount> &mounts)
        : Fl_Window(720, 360, "Select USB Drive"), mounts_(mounts)
    {
        set_modal();
        Fl_Box *msg = new Fl_Box(10, 10, 700, 45,
            "Multiple removable FAT/exFAT drives were found. Select the drive to use.");
        msg->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

        browser_ = new Fl_Hold_Browser(10, 60, 700, 230);
        static int widths[] = {300, 90, 130, 0};
        browser_->column_widths(widths);
        browser_->column_char('\t');
        browser_->add("Mount path\tFilesystem\tFree space");
        for (const auto &m : mounts_) {
            std::ostringstream os;
            os << m.path << '\t' << m.fs_type << '\t' << m.free_text;
            browser_->add(os.str().c_str());
        }
        if (!mounts_.empty())
            browser_->select(2); // row 1 is header

        Fl_Button *cancel = new Fl_Button(500, 310, 90, 30, "Cancel");
        cancel->callback(cbCancel, this);
        Fl_Button *use = new Fl_Button(600, 310, 110, 30, "Use Selected");
        use->callback(cbUse, this);
        end();
    }

    std::optional<UsbMount> selected() const { return selected_; }

private:
    const std::vector<UsbMount> &mounts_;
    Fl_Hold_Browser *browser_ = nullptr;
    std::optional<UsbMount> selected_;

    static void cbCancel(Fl_Widget *, void *data)
    {
        static_cast<UsbChooserWindow *>(data)->hide();
    }

    static void cbUse(Fl_Widget *, void *data)
    {
        UsbChooserWindow *self = static_cast<UsbChooserWindow *>(data);
        const int row = self->browser_->value();
        const int idx = row - 2; // row 1 is header
        if (idx < 0 || static_cast<size_t>(idx) >= self->mounts_.size()) {
            fl_alert("Select a USB drive first.");
            return;
        }
        self->selected_ = self->mounts_[idx];
        self->hide();
    }
};

static std::optional<UsbMount> chooseUsbMount(const std::vector<UsbMount> &mounts)
{
    UsbChooserWindow chooser(mounts);
    chooser.show();
    while (chooser.shown())
        Fl::wait();
    return chooser.selected();
}

class DetailsWindow : public Fl_Window {
public:
    DetailsWindow(const ConfigItem &item) : Fl_Window(620, 420, "Configuration Details")
    {
        buffer_ = new Fl_Text_Buffer();
        display_ = new Fl_Text_Display(10, 10, 600, 360);
        display_->buffer(buffer_);
        Fl_Button *close = new Fl_Button(520, 380, 90, 30, "Close");
        close->callback([](Fl_Widget *, void *data) { static_cast<DetailsWindow *>(data)->hide(); }, this);
        end();
        buffer_->text(buildText(item).c_str());
        set_modal();
    }
    ~DetailsWindow() override { delete buffer_; }

private:
    Fl_Text_Buffer *buffer_ = nullptr;
    Fl_Text_Display *display_ = nullptr;

    static std::string buildText(const ConfigItem &item)
    {
        std::ostringstream os;
        os << "EEPROM file: " << item.eeprom_path << "\n";
        os << "Sidecar: " << item.sidecar_path << "\n\n";
        os << "Name: " << item.name << "\n";
        os << "Created: " << item.created << "\n";
        os << "Hostname: " << item.from << "\n";
        os << "HamClock version: " << item.version << "\n";
        os << "Size: " << item.size_text << "\n";
        auto crc = Crc32::file(item.eeprom_path);
        os << "Current CRC32: " << (crc ? Crc32::hex(*crc) : std::string("unavailable")) << "\n";
        os << "Metadata: " << (item.has_metadata ? "present" : "missing or invalid") << "\n\n";
        if (item.info.present) {
            os << "Raw sidecar fields:\n";
            for (const auto &kv : item.info.kv)
                os << "  " << kv.first << " = " << kv.second << "\n";
        } else {
            os << "No sidecar metadata file was found. This is allowed.\n";
        }
        return os.str();
    }
};

class App {
public:
    App()
    {
        const char *home = getenv("HOME");
        home_dir_ = home ? fs::path(home) : fs::path(".");
        hamclock_dir_ = home_dir_ / ".hamclock";
        config_dir_ = hamclock_dir_ / "configurations";
        live_eeprom_ = hamclock_dir_ / "eeprom";

        win_ = new Fl_Window(760, 560, "HamClock Backup & Restore");
        usb_box_ = new Fl_Box(10, 10, 560, 25, "USB drive: scanning...");
        usb_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        refresh_btn_ = new Fl_Button(600, 10, 140, 25, "Refresh");
        refresh_btn_->callback(cbRefresh, this);

        local_label_ = new Fl_Box(10, 45, 350, 20, "Local configurations (~/.hamclock/configurations)");
        local_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        local_browser_ = new Fl_Multi_Browser(10, 70, 740, 170);
        static int widths[] = {180, 180, 150, 80, 0};
        local_browser_->column_widths(widths);
        local_browser_->column_char('\t');

        backup_btn_ = new Fl_Button(485, 250, 265, 30, "Back up selected to USB ->");
        backup_btn_->callback(cbBackup, this);

        usb_label_ = new Fl_Box(10, 290, 350, 20, "Configurations on USB");
        usb_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        usb_browser_ = new Fl_Multi_Browser(10, 315, 740, 170);
        usb_browser_->column_widths(widths);
        usb_browser_->column_char('\t');

        restore_btn_ = new Fl_Button(10, 495, 240, 30, "<- Restore selected from USB");
        restore_btn_->callback(cbRestore, this);
        delete_usb_btn_ = new Fl_Button(260, 495, 160, 30, "Delete from USB");
        delete_usb_btn_->callback(cbDeleteUsb, this);
        details_btn_ = new Fl_Button(430, 495, 140, 30, "Details");
        details_btn_->callback(cbDetails, this);

        status_box_ = new Fl_Box(10, 530, 740, 20, "Status: Ready.");
        status_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        win_->end();
        win_->resizable(win_);
        refresh();
    }

    int run(int argc, char **argv)
    {
        win_->show(argc, argv);
        return Fl::run();
    }

private:
    fs::path home_dir_, hamclock_dir_, config_dir_, live_eeprom_;
    std::vector<ConfigItem> local_items_;
    std::vector<ConfigItem> usb_items_;
    std::vector<UsbMount> mounts_;
    std::optional<UsbMount> usb_;

    Fl_Window *win_ = nullptr;
    Fl_Box *usb_box_ = nullptr;
    Fl_Box *local_label_ = nullptr;
    Fl_Box *usb_label_ = nullptr;
    Fl_Box *status_box_ = nullptr;
    Fl_Multi_Browser *local_browser_ = nullptr;
    Fl_Multi_Browser *usb_browser_ = nullptr;
    Fl_Button *refresh_btn_ = nullptr;
    Fl_Button *backup_btn_ = nullptr;
    Fl_Button *restore_btn_ = nullptr;
    Fl_Button *delete_usb_btn_ = nullptr;
    Fl_Button *details_btn_ = nullptr;

    static void cbRefresh(Fl_Widget *, void *data) { static_cast<App *>(data)->refresh(); }
    static void cbBackup(Fl_Widget *, void *data) { static_cast<App *>(data)->backupSelected(); }
    static void cbRestore(Fl_Widget *, void *data) { static_cast<App *>(data)->restoreSelected(); }
    static void cbDeleteUsb(Fl_Widget *, void *data) { static_cast<App *>(data)->deleteUsbSelected(); }
    static void cbDetails(Fl_Widget *, void *data) { static_cast<App *>(data)->showDetails(); }

    void setStatus(const std::string &s)
    {
        status_box_->copy_label(("Status: " + s).c_str());
        status_box_->redraw();
        Fl::check();
    }

    std::string rowText(const ConfigItem &item) const
    {
        std::ostringstream os;
        os << item.name << '\t' << item.created << '\t' << item.from << '\t' << item.size_text;
        return os.str();
    }

    void fillBrowser(Fl_Multi_Browser *browser, const std::vector<ConfigItem> &items)
    {
        browser->clear();
        browser->add("Name\tCreated\tFrom\tSize");
        for (const auto &item : items)
            browser->add(rowText(item).c_str());
    }

    void refresh()
    {
        local_items_ = scanConfigs(config_dir_);
        const std::optional<UsbMount> previous_usb = usb_;
        mounts_ = findUsbMounts();
        usb_.reset();
        usb_items_.clear();

        if (mounts_.empty()) {
            usb_box_->copy_label("USB drive: none found. Insert one FAT/exFAT USB drive and press Refresh.");
        } else if (previous_usb) {
            // Keep using the drive the user already selected, if it is still
            // mounted. This prevents copy/restore/delete follow-up refreshes
            // from repeatedly opening the chooser when multiple candidates
            // remain mounted.
            auto it = std::find_if(mounts_.begin(), mounts_.end(), [&](const UsbMount &m) {
                return m.path == previous_usb->path;
            });
            if (it != mounts_.end()) {
                usb_ = *it;
            }
        }

        if (!usb_) {
            if (mounts_.size() > 1) {
                usb_ = chooseUsbMount(mounts_);
                if (!usb_) {
                    usb_box_->copy_label("USB drive: multiple FAT/exFAT drives found. No drive selected.");
                }
            } else if (mounts_.size() == 1) {
                usb_ = mounts_[0];
            }
        }

        if (usb_) {
            std::string label = "USB drive: " + usb_->path + " (" + usb_->fs_type;
            if (!usb_->free_text.empty())
                label += ", " + usb_->free_text;
            label += ")";
            usb_box_->copy_label(label.c_str());
            usb_items_ = scanConfigs(usb_->path);
        }

        fillBrowser(local_browser_, local_items_);
        fillBrowser(usb_browser_, usb_items_);
        const bool usb_ok = usb_.has_value();
        backup_btn_->activate();
        restore_btn_->activate();
        delete_usb_btn_->activate();
        if (!usb_ok) {
            backup_btn_->deactivate();
            restore_btn_->deactivate();
            delete_usb_btn_->deactivate();
        }
        setStatus("Ready.");
    }

    std::vector<int> selectedDataIndexes(Fl_Multi_Browser *browser, size_t max_items) const
    {
        std::vector<int> indexes;
        for (int i = 2; i <= browser->size(); i++) { // row 1 is header
            if (browser->selected(i)) {
                int idx = i - 2;
                if (idx >= 0 && static_cast<size_t>(idx) < max_items)
                    indexes.push_back(idx);
            }
        }
        return indexes;
    }

    bool blockIfHamClockRunning()
    {
        if (isHamClockRunning(live_eeprom_)) {
            fl_alert("HamClock appears to be running.\n\nClose HamClock before backup or restore so files are not copied while active.");
            setStatus("Blocked because HamClock is running.");
            return true;
        }
        return false;
    }

    bool ensureUsbReady()
    {
        if (!usb_) {
            fl_alert("No single FAT/exFAT USB drive is available.\n\nInsert one USB drive, or remove extras, then press Refresh.");
            return false;
        }
        return true;
    }

    bool confirmOverwrite(const fs::path &dst, const char *action)
    {
        std::error_code ec;
        if (!fs::exists(dst, ec))
            return true;
        int rc = fl_choice("%s already exists:\n%s\n\nReplace it?", "Skip", "Replace", nullptr, action, dst.c_str());
        return rc == 1;
    }

    void backupSelected()
    {
        if (!ensureUsbReady() || blockIfHamClockRunning())
            return;
        auto indexes = selectedDataIndexes(local_browser_, local_items_.size());
        if (indexes.empty()) {
            fl_alert("Select one or more local configurations to back up.");
            return;
        }

        int ok = 0, failed = 0, skipped = 0;
        for (int idx : indexes) {
            const ConfigItem &item = local_items_[idx];
            const fs::path dst = fs::path(usb_->path) / item.eeprom_path.filename();
            const fs::path dst_sidecar = fs::path(dst.string() + HC_INFO_SUFFIX);
            if (!confirmOverwrite(dst, "USB backup")) {
                skipped++;
                continue;
            }
            setStatus("Backing up " + item.name + "...");
            std::string err;
            if (!copyFileWithCrcVerify(item.eeprom_path, dst, err) || !copySidecarIfPresent(item.sidecar_path, dst_sidecar, err)) {
                fl_alert("Backup failed for %s:\n%s", item.name.c_str(), err.c_str());
                failed++;
            } else {
                ok++;
            }
        }
        refresh();
        setStatus("Backup complete. OK=" + std::to_string(ok) + ", failed=" + std::to_string(failed) + ", skipped=" + std::to_string(skipped) + ".");
    }

    void restoreSelected()
    {
        if (!ensureUsbReady() || blockIfHamClockRunning())
            return;
        auto indexes = selectedDataIndexes(usb_browser_, usb_items_.size());
        if (indexes.empty()) {
            fl_alert("Select one or more USB configurations to restore.");
            return;
        }

        int ok = 0, failed = 0, skipped = 0;
        std::error_code ec;
        fs::create_directories(config_dir_, ec);
        if (ec) {
            fl_alert("Could not create local configuration directory:\n%s", ec.message().c_str());
            return;
        }

        for (int idx : indexes) {
            const ConfigItem &item = usb_items_[idx];
            const fs::path dst = config_dir_ / item.eeprom_path.filename();
            const fs::path dst_sidecar = fs::path(dst.string() + HC_INFO_SUFFIX);
            if (!confirmOverwrite(dst, "Local configuration")) {
                skipped++;
                continue;
            }
            const std::string installed_version = HAMCLOCK_VERSION;
            if (!installed_version.empty() && !item.version.empty() && item.version != installed_version) {
                int vr = fl_choice("%s was created with HamClock %s.\n\nThis app was built for HamClock %s. Restore anyway?",
                                   "Cancel", "Restore Anyway", nullptr,
                                   item.name.c_str(), item.version.c_str(), installed_version.c_str());
                if (vr != 1) {
                    skipped++;
                    continue;
                }
            }
            setStatus("Restoring " + item.name + "...");
            std::string err;
            if (!copyFileWithCrcVerify(item.eeprom_path, dst, err) || !copySidecarIfPresent(item.sidecar_path, dst_sidecar, err)) {
                fl_alert("Restore failed for %s:\n%s", item.name.c_str(), err.c_str());
                failed++;
            } else {
                ok++;
            }
        }
        refresh();
        setStatus("Restore complete. OK=" + std::to_string(ok) + ", failed=" + std::to_string(failed) + ", skipped=" + std::to_string(skipped) + ".");
    }

    void deleteUsbSelected()
    {
        if (!ensureUsbReady() || blockIfHamClockRunning())
            return;
        auto indexes = selectedDataIndexes(usb_browser_, usb_items_.size());
        if (indexes.empty()) {
            fl_alert("Select one or more USB configurations to delete.");
            return;
        }
        int rc = fl_choice("Delete %zu selected configuration(s) from the USB drive?", "Cancel", "Delete", nullptr, indexes.size());
        if (rc != 1)
            return;

        int ok = 0, failed = 0;
        for (int idx : indexes) {
            const ConfigItem &item = usb_items_[idx];
            std::error_code ec1, ec2;
            fs::remove(item.eeprom_path, ec1);
            fs::remove(item.sidecar_path, ec2);
            if (ec1) {
                fl_alert("Delete failed for %s:\n%s", item.name.c_str(), ec1.message().c_str());
                failed++;
            } else {
                ok++;
            }
        }
        refresh();
        setStatus("USB delete complete. OK=" + std::to_string(ok) + ", failed=" + std::to_string(failed) + ".");
    }

    void showDetails()
    {
        auto local_sel = selectedDataIndexes(local_browser_, local_items_.size());
        auto usb_sel = selectedDataIndexes(usb_browser_, usb_items_.size());
        if (!local_sel.empty()) {
            DetailsWindow *w = new DetailsWindow(local_items_[local_sel.front()]);
            w->show();
            return;
        }
        if (!usb_sel.empty()) {
            DetailsWindow *w = new DetailsWindow(usb_items_[usb_sel.front()]);
            w->show();
            return;
        }
        fl_alert("Select a configuration first.");
    }
};

int main(int argc, char **argv)
{
    App app;
    return app.run(argc, argv);
}
