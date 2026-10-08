// SPDX-License-Identifier: GPL-3.0-or-later

#include "main_window.hpp"
#include "application.hpp"
#include "test_hooks.hpp"

#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <giomm/fileinputstream.h>
#include <glib.h>
#include <glibmm/bytes.h>
#include <glibmm/convert.h>
#include <glibmm/fileutils.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/fontchooserdialog.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/pagesetup.h>
#include <gtkmm/printoperation.h>
#include <gtkmm/printsettings.h>
#include <gtkmm/radiobutton.h>
#include <gtkmm/settings.h>
#include <gtkmm/stock.h>
#include <gtkmm/stylecontext.h>

#ifdef LUNDUKE_EDIT_TEST_HOOKS
#include <gdk/gdkx.h>
#endif

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <X11/Xlib.h>

namespace lundukeedit {
namespace {

using NewlineStyle = MainWindow::NewlineStyle;

const char* kVersion = "0.9";
constexpr const char* kAppId = "org.lunduke.LundukeEdit";
constexpr const char* kFallbackIcon = "accessories-text-editor";

// Prefer shipped hicolor app id (Paint pattern); fall back to freedesktop
// text-editor name when Bob's artwork is not installed yet.
Glib::ustring resolve_app_icon_name() {
  auto theme = Gtk::IconTheme::get_default();
  if (theme && theme->has_icon(kAppId)) {
    return kAppId;
  }
  return kFallbackIcon;
}

std::string format_bytes(std::size_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  const int len = static_cast<int>(digits.size());
  for (int i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[static_cast<std::size_t>(i)]);
  }
  out += (n == 1) ? " byte" : " bytes";
  return out;
}

constexpr std::size_t kDefaultMaxOpenBytes = 32u * 1024u * 1024u;
constexpr std::size_t kDefaultMaxPasteBytes = 32u * 1024u * 1024u;
// Files above the 32 MiB confirm threshold may still be opened, up to this
// ceiling. There is no "open anyway" past it: the UI thread must not read
// an unbounded file into a GtkTextBuffer.
constexpr std::size_t kDefaultMaxOpenHardBytes = 64u * 1024u * 1024u;
constexpr int kMaxColumnWalk = 4096;
constexpr int kLongLineChars = 4000;
// Characters of a long line that stay visible around the caret. The rest is
// tagged invisible so a keystroke does not shape the whole line. 4096 is
// about the width GDK can scroll (windows stop at 32767 pixels).
constexpr int kLongLineWindow = 4096;
constexpr gsize kLoadReadBytes = 256u * 1024u;
constexpr int kFindSliceChars = 64 * 1024;
constexpr int kDefaultMaxFindHits = 10000;
constexpr int kDefaultFindChunk = 200;
constexpr std::size_t kDefaultHugeUndoBytes = 8u * 1024u * 1024u;

std::size_t max_open_bytes() {
  return test_max_open_bytes(kDefaultMaxOpenBytes);
}

std::size_t max_open_hard_bytes() {
  return test_max_open_hard_bytes(kDefaultMaxOpenHardBytes);
}

std::string hard_open_limit_phrase(std::size_t hard) {
  const std::size_t mib = 1024u * 1024u;
  if (hard >= mib && hard % mib == 0) {
    return std::to_string(hard / mib) + " MiB";
  }
  return format_bytes(hard);
}

std::string hard_open_refusal(std::size_t hard) {
  return "The file is larger than " + hard_open_limit_phrase(hard) +
         ". It will not be opened.";
}

std::size_t max_paste_bytes() {
  return test_max_paste_bytes(kDefaultMaxPasteBytes);
}

int max_find_hits() {
  return test_max_find_hits(kDefaultMaxFindHits);
}

int find_chunk_size() {
  return test_find_chunk(kDefaultFindChunk);
}

std::size_t huge_undo_limit() {
  return test_huge_undo_bytes(kDefaultHugeUndoBytes);
}

std::size_t count_newlines(const char* data, std::size_t len) {
  return static_cast<std::size_t>(
      std::count(data, data + len, '\n'));
}

bool has_long_line(const char* data, std::size_t len, std::size_t limit) {
  std::size_t run = 0;
  for (std::size_t i = 0; i < len; ++i) {
    if (data[i] == '\n') {
      run = 0;
    } else if (++run >= limit) {
      return true;
    }
  }
  return false;
}

// Buffer text uses LF. `kinds` is one entry per buffer line: 'n' LF, 'c' CRLF,
// 'r' CR, or 0 when that line has no break. Mixed files keep each line's
// own break instead of restyling the whole buffer.
NewlineStyle normalize_newlines(std::string& raw, std::vector<char>& kinds) {
  kinds.clear();
  std::size_t crlf = 0;
  std::size_t lf = 0;
  std::size_t cr = 0;
  std::string out;
  out.reserve(raw.size());
  std::string cur;
  auto push_line = [&](char kind) {
    out.append(cur);
    kinds.push_back(kind);
    if (kind != 0) {
      out.push_back('\n');
    }
    cur.clear();
  };
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '\r') {
      if (i + 1 < raw.size() && raw[i + 1] == '\n') {
        ++crlf;
        ++i;
        push_line('c');
      } else {
        ++cr;
        push_line('r');
      }
    } else if (c == '\n') {
      ++lf;
      push_line('n');
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    push_line(0);
  }
  raw.swap(out);
  NewlineStyle style = NewlineStyle::Lf;
  if (crlf > 0 && crlf >= lf && crlf >= cr) {
    style = NewlineStyle::Crlf;
  } else if (cr > 0 && cr > lf) {
    style = NewlineStyle::Cr;
  }
  return style;
}

std::string apply_newline_style(const std::string& in, NewlineStyle style) {
  if (style == NewlineStyle::Lf) {
    return in;
  }
  std::string out;
  out.reserve(in.size() + (style == NewlineStyle::Crlf ? in.size() / 8 : 0));
  for (char c : in) {
    if (c == '\n') {
      if (style == NewlineStyle::Crlf) {
        out.push_back('\r');
        out.push_back('\n');
      } else {
        out.push_back('\r');
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool write_all_fd(int fd, const std::string& bytes, std::string& error,
                  bool inplace_copy) {
  const char* p = bytes.data();
  std::size_t left = bytes.size();
#ifndef LUNDUKE_EDIT_TEST_HOOKS
  (void)inplace_copy;
#endif
  while (left > 0) {
    ssize_t n = 0;
#ifdef LUNDUKE_EDIT_TEST_HOOKS
    if (inplace_copy && MainWindow::test_write_hook_ != nullptr) {
      n = MainWindow::test_write_hook_(fd, p, left, true);
    } else {
      n = ::write(fd, p, left);
    }
#else
    n = ::write(fd, p, left);
#endif
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::string("Could not write file: ") + std::strerror(errno);
      return false;
    }
    if (n == 0) {
      error = "Could not write file.";
      return false;
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) {
    error = std::string("Could not flush file to disk: ") + std::strerror(errno);
    return false;
  }
  return true;
}

struct SavedXattr {
  std::string name;
  std::string value;
};

std::vector<SavedXattr> read_xattrs(int fd) {
  std::vector<SavedXattr> out;
  if (fd < 0) {
    return out;
  }
  ssize_t size = flistxattr(fd, nullptr, 0);
  if (size <= 0 || size > 1024 * 1024) {
    return out;
  }
  std::vector<char> names(static_cast<std::size_t>(size));
  size = flistxattr(fd, names.data(), names.size());
  if (size < 0) {
    return out;
  }
  for (ssize_t i = 0; i < size;) {
    const char* name = names.data() + i;
    const std::size_t nlen = std::strlen(name);
    i += static_cast<ssize_t>(nlen + 1);
    if (nlen == 0) {
      break;
    }
    const ssize_t vlen = fgetxattr(fd, name, nullptr, 0);
    if (vlen < 0 || vlen > 1024 * 1024) {
      continue;
    }
    std::string value(static_cast<std::size_t>(vlen), '\0');
    if (fgetxattr(fd, name, value.data(), value.size()) < 0) {
      continue;
    }
    out.push_back(SavedXattr{name, std::move(value)});
  }
  return out;
}

void write_xattrs(int fd, const std::vector<SavedXattr>& attrs) {
  if (fd < 0) {
    return;
  }
  for (const auto& attr : attrs) {
    if (fsetxattr(fd, attr.name.c_str(), attr.value.data(), attr.value.size(),
                  0) != 0) {
      // security.* often needs a privilege the editor does not have.
    }
  }
}

struct OwnedFd {
  int fd{-1};
  explicit OwnedFd(int f = -1) : fd(f) {}
  ~OwnedFd() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  OwnedFd(OwnedFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
  int get() const { return fd; }
  int release() {
    const int f = fd;
    fd = -1;
    return f;
  }
};

void fsync_parent_best_effort(int dirfd) {
  if (dirfd < 0) {
    return;
  }
  int rc = 0;
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  if (MainWindow::test_dir_fsync_hook_ != nullptr) {
    rc = MainWindow::test_dir_fsync_hook_(dirfd);
  } else {
    rc = ::fsync(dirfd);
  }
#else
  rc = ::fsync(dirfd);
#endif
  if (rc != 0) {
    const int err = errno;
    // EINVAL and EROFS mean the filesystem has no directory flush.
    // EOPNOTSUPP and ENOSYS are the same kind of "not supported".
    if (err != EINVAL && err != EROFS && err != EOPNOTSUPP && err != ENOSYS) {
      g_warning("Could not flush directory after save: %s", std::strerror(err));
    }
  }
}

int make_temp_file(const std::string& dir, const std::string& base,
                   std::string& out_path, std::string& error) {
  std::string name;
  if (base.size() + 8 > static_cast<std::size_t>(NAME_MAX)) {
    name = ".leXXXXXX";
  } else {
    name = "." + base + ".XXXXXX";
  }
  const std::string pattern = Glib::build_filename(dir, name);
  std::vector<char> tmpl(pattern.begin(), pattern.end());
  tmpl.push_back('\0');
  const int fd = mkstemp(tmpl.data());
  if (fd < 0) {
    error = std::string("Could not write temporary file: ") + std::strerror(errno);
    return -1;
  }
  out_path.assign(tmpl.data());
  return fd;
}

int make_temp_anywhere(std::string& out_path, std::string& error) {
  const std::string pattern =
      Glib::build_filename(Glib::get_tmp_dir(), ".leXXXXXX");
  std::vector<char> tmpl(pattern.begin(), pattern.end());
  tmpl.push_back('\0');
  const int fd = mkstemp(tmpl.data());
  if (fd < 0) {
    error = std::string("Could not write temporary file: ") + std::strerror(errno);
    return -1;
  }
  out_path.assign(tmpl.data());
  return fd;
}

mode_t new_file_mode() {
  const mode_t mask = umask(0);
  umask(mask);
  return static_cast<mode_t>(0666 & ~mask);
}

// Stage the full text, fsync it, then copy into the existing inode. A short
// write can still tear that inode; the error names the temp file and says so.
bool write_in_place_staged(int dirfd, const std::string& dir,
                           const std::string& base, const struct stat& st,
                           const std::string& bytes, std::string& error) {
  std::string tmp_path;
  int tmp = make_temp_file(dir, base, tmp_path, error);
  if (tmp < 0) {
    const int mk_err = errno;
    if (mk_err != EACCES && mk_err != ENAMETOOLONG) {
      return false;
    }
    tmp = make_temp_anywhere(tmp_path, error);
    if (tmp < 0) {
      return false;
    }
  }
  OwnedFd tmpfd(tmp);
  std::string write_error;
  if (!write_all_fd(tmpfd.get(), bytes, write_error, false)) {
    ::unlink(tmp_path.c_str());
    error = write_error;
    return false;
  }

  OwnedFd dest(::openat(dirfd, base.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
  if (dest.get() < 0) {
    error = std::string("Could not write file: ") + std::strerror(errno) +
            ". A complete copy was left at " + tmp_path + ".";
    return false;
  }
  struct stat now {};
  if (::fstat(dest.get(), &now) != 0 || !S_ISREG(now.st_mode) ||
      now.st_ino != st.st_ino || now.st_dev != st.st_dev) {
    error = "Could not write file: it changed during save. A complete copy was "
            "left at " +
            tmp_path + ".";
    return false;
  }
  if (!write_all_fd(dest.get(), bytes, write_error, true)) {
    error = "The file may be damaged. A complete copy was left at " + tmp_path +
            ". " + write_error;
    return false;
  }
  if (::ftruncate(dest.get(), static_cast<off_t>(bytes.size())) != 0) {
    error = std::string("The file may be damaged. A complete copy was left at ") +
            tmp_path + ". " + std::strerror(errno);
    return false;
  }
  if (::fsync(dest.get()) != 0) {
    error = std::string("The file may be damaged. A complete copy was left at ") +
            tmp_path + ". " + std::strerror(errno);
    return false;
  }
  ::unlink(tmp_path.c_str());
  return true;
}

bool replace_following(const std::string& path, const std::string& bytes,
                       std::string& error, bool allow_symlink);

bool save_through_symlink(const std::string& path, const std::string& bytes,
                          std::string& error, bool allow_symlink) {
  if (!allow_symlink) {
    error = "Could not replace file: refusing to follow a symlink.";
    return false;
  }
  char* canon = ::realpath(path.c_str(), nullptr);
  if (canon == nullptr) {
    error = std::string("Could not resolve symlink: ") + std::strerror(errno);
    return false;
  }
  const std::string target(canon);
  std::free(canon);
  return replace_following(target, bytes, error, false);
}

// Open the parent, then the final component with O_NOFOLLOW. A symlink is
// resolved once and the target is opened the same way; a symlink there is
// refused. The temp file stays mode 0600 until rename, then the destination
// is chmodded. Directory fsync failure after a successful rename is logged
// and is not a failed save.
bool replace_following(const std::string& path, const std::string& bytes,
                       std::string& error, bool allow_symlink) {
  const std::string dir = Glib::path_get_dirname(path);
  const std::string base = Glib::path_get_basename(path);
  if (base.empty() || base == "." || base == "..") {
    error = "Could not replace file.";
    return false;
  }
  OwnedFd dirfd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dirfd.get() < 0) {
    error = std::string("Could not open directory: ") + std::strerror(errno);
    return false;
  }

  OwnedFd pathfd(
      ::openat(dirfd.get(), base.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
  if (pathfd.get() < 0 && errno == ELOOP) {
    return save_through_symlink(path, bytes, error, allow_symlink);
  }

  struct stat st {};
  bool existed = false;
  if (pathfd.get() >= 0) {
    if (::fstat(pathfd.get(), &st) != 0) {
      error = std::string("Could not stat file: ") + std::strerror(errno);
      return false;
    }
    if (S_ISLNK(st.st_mode)) {
      return save_through_symlink(path, bytes, error, allow_symlink);
    }
    if (!S_ISREG(st.st_mode)) {
      error = "Could not replace file: not a regular file.";
      return false;
    }
    existed = true;
  } else if (errno != ENOENT) {
    error = std::string("Could not open file: ") + std::strerror(errno);
    return false;
  }

  if (existed && st.st_nlink > 1) {
    return write_in_place_staged(dirfd.get(), dir, base, st, bytes, error);
  }

  std::string tmp_path;
  const int tmp = make_temp_file(dir, base, tmp_path, error);
  if (tmp < 0) {
    const int mk_err = errno;
    if (existed && (mk_err == EACCES || mk_err == ENAMETOOLONG)) {
      return write_in_place_staged(dirfd.get(), dir, base, st, bytes, error);
    }
    return false;
  }
  OwnedFd tmpfd(tmp);

  std::vector<SavedXattr> attrs;
  if (existed) {
    if (::fchown(tmpfd.get(), st.st_uid, st.st_gid) != 0) {
      // EPERM is normal when the file is owned by someone else.
    }
    OwnedFd src(::openat(dirfd.get(), base.c_str(),
                         O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    attrs = read_xattrs(src.get());
    write_xattrs(tmpfd.get(), attrs);
  }

  if (!write_all_fd(tmpfd.get(), bytes, error, false)) {
    ::unlink(tmp_path.c_str());
    return false;
  }
  if (::close(tmpfd.release()) != 0) {
    error = std::string("Could not write temporary file: ") + std::strerror(errno);
    ::unlink(tmp_path.c_str());
    return false;
  }

  if (existed) {
    OwnedFd check(::openat(dirfd.get(), base.c_str(),
                           O_PATH | O_NOFOLLOW | O_CLOEXEC));
    struct stat chk {};
    if (check.get() < 0 || ::fstat(check.get(), &chk) != 0 ||
        S_ISLNK(chk.st_mode) || chk.st_ino != st.st_ino ||
        chk.st_dev != st.st_dev) {
      ::unlink(tmp_path.c_str());
      error = "Could not replace file: it changed during save.";
      return false;
    }
  } else {
    OwnedFd check(::openat(dirfd.get(), base.c_str(),
                           O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (check.get() >= 0) {
      struct stat chk {};
      if (::fstat(check.get(), &chk) == 0 && S_ISLNK(chk.st_mode)) {
        ::unlink(tmp_path.c_str());
        error = "Could not replace file: refusing to follow a symlink.";
        return false;
      }
    } else if (errno == ELOOP) {
      ::unlink(tmp_path.c_str());
      error = "Could not replace file: refusing to follow a symlink.";
      return false;
    }
  }

  const std::string tmp_base = Glib::path_get_basename(tmp_path);
  if (::renameat(dirfd.get(), tmp_base.c_str(), dirfd.get(), base.c_str()) !=
      0) {
    error = std::string("Could not replace file: ") + std::strerror(errno);
    ::unlink(tmp_path.c_str());
    return false;
  }

  const mode_t mode =
      existed ? static_cast<mode_t>(st.st_mode & 0777) : new_file_mode();
  OwnedFd dest(
      ::openat(dirfd.get(), base.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
  if (dest.get() >= 0) {
    if (::fchmod(dest.get(), mode) != 0) {
      g_warning("Could not restore file mode: %s", std::strerror(errno));
    }
    write_xattrs(dest.get(), attrs);
  }

  // rename already published the new bytes. A directory flush failure must
  // not be reported as a failed save.
  fsync_parent_best_effort(dirfd.get());
  return true;
}

bool replace_file_contents(const std::string& path, const std::string& bytes,
                           std::string& error) {
  return replace_following(path, bytes, error, true);
}

// Secondary text for an open failure: the reason, then the path on its own
// line. errno is captured at the failing call; a later close must not replace it.
std::string open_failure_text(const std::string& path, int err, bool non_regular,
                              bool is_directory) {
  const char* reason = nullptr;
  if (is_directory || err == EISDIR) {
    reason = "That path is a directory";
  } else if (non_regular) {
    reason = "That path is not a regular file";
  } else if (err == EACCES) {
    reason = "Permission denied";
  } else if (err == ENOENT) {
    reason = "No such file";
  } else if (err != 0) {
    reason = std::strerror(err);
  }
  if (reason == nullptr || reason[0] == '\0') {
    return path;
  }
  return std::string(reason) + "\n" + path;
}

bool bytes_contain_nul(const std::string& bytes) {
  return bytes.find('\0') != std::string::npos;
}

bool text_contains_nul(const Glib::ustring& text) {
  return text.bytes() != std::strlen(text.c_str()) ||
         text.find('\0') != Glib::ustring::npos;
}

std::string canonical_path(const std::string& path) {
  if (path.empty()) {
    return path;
  }
  char* canon = ::realpath(path.c_str(), nullptr);
  if (canon == nullptr) {
    return path;
  }
  std::string out(canon);
  std::free(canon);
  return out;
}

Glib::ustring strip_carriage_returns(const Glib::ustring& in) {
  std::string raw(in.data(), in.bytes());
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r') {
      if (i + 1 < raw.size() && raw[i + 1] == '\n') {
        continue;
      }
      out.push_back('\n');
    } else {
      out.push_back(raw[i]);
    }
  }
  return Glib::ustring(out);
}

struct LinePart {
  std::string text;
  bool nl{false};
};

std::vector<LinePart> split_lf_lines(const std::string& s) {
  std::vector<LinePart> parts;
  std::size_t i = 0;
  while (i < s.size()) {
    const std::size_t nl = s.find('\n', i);
    if (nl == std::string::npos) {
      parts.push_back(LinePart{s.substr(i), false});
      break;
    }
    parts.push_back(LinePart{s.substr(i, nl - i), true});
    i = nl + 1;
    if (i == s.size()) {
      break;
    }
  }
  return parts;
}

char style_kind(NewlineStyle style) {
  if (style == NewlineStyle::Crlf) {
    return 'c';
  }
  if (style == NewlineStyle::Cr) {
    return 'r';
  }
  return 'n';
}

struct SavedLine {
  std::string text;
  char kind{0};
};

std::string apply_line_endings(const std::string& encoded_lf,
                               const std::vector<SavedLine>& source,
                               NewlineStyle style) {
  const auto enc = split_lf_lines(encoded_lf);
  std::string out;
  out.reserve(encoded_lf.size() + encoded_lf.size() / 16);
  for (std::size_t i = 0; i < enc.size(); ++i) {
    out.append(enc[i].text);
    if (!enc[i].nl) {
      continue;
    }
    char kind = style_kind(style);
    if (i < source.size()) {
      kind = source[i].kind == 0 ? style_kind(style) : source[i].kind;
    }
    if (kind == 'c') {
      out.append("\r\n");
    } else if (kind == 'r') {
      out.push_back('\r');
    } else {
      out.push_back('\n');
    }
  }
  return out;
}

enum class DiskIdentity { Unchanged, Changed, Missing, Error };

struct DiskCheck {
  DiskIdentity kind{DiskIdentity::Unchanged};
  std::string message;
};

// Compares the path this window loaded with the file that is there now.
// ENOENT is Missing (the buffer may be the only copy). Any other stat
// failure is Error. Both used to look like "unchanged", so a clean close
// dropped the text and a clean save reported success without writing.
DiskCheck file_on_disk(const std::string& path, const std::string& loaded,
                       bool have_id, std::uint64_t dev, std::uint64_t ino,
                       std::int64_t sec, std::int64_t nsec) {
  DiskCheck out;
  if (!have_id || loaded.empty()) {
    return out;
  }
  if (canonical_path(path) != canonical_path(loaded)) {
    return out;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) {
      out.kind = DiskIdentity::Missing;
      out.message = "This file was deleted.";
      return out;
    }
    out.kind = DiskIdentity::Error;
    out.message =
        std::string("Could not read file information: ") + std::strerror(errno);
    return out;
  }
  if (static_cast<std::uint64_t>(st.st_dev) != dev ||
      static_cast<std::uint64_t>(st.st_ino) != ino ||
      static_cast<std::int64_t>(st.st_mtim.tv_sec) != sec ||
      static_cast<std::int64_t>(st.st_mtim.tv_nsec) != nsec) {
    out.kind = DiskIdentity::Changed;
  }
  return out;
}

std::string css_font_family(const std::string& family) {
  std::string out;
  out.reserve(family.size());
  for (const char c : family) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

// GtkTextView reads its layout font from the textview style node. The
// text child is the node that paints the glyphs. Both have to carry the
// face, or a theme rule on one of them wins.
std::string editor_font_css(const Pango::FontDescription& desc) {
  std::string family = desc.get_family();
  if (family.empty()) {
    family = "Monospace";
  }
  double points = 11.0;
  if (desc.get_size() > 0) {
    points = static_cast<double>(desc.get_size()) / Pango::SCALE;
    if (desc.get_size_is_absolute()) {
      points = points * 72.0 / 96.0;
    }
  }
  if (points < 1.0) {
    points = 11.0;
  }
  const int whole = static_cast<int>(std::lround(points));
  return "textview, textview text {\n  font-family: \"" +
         css_font_family(family) + "\";\n  font-size: " +
         std::to_string(whole) + "pt;\n}\n";
}

template <typename Dialog>
int run_modal(Application& app, Dialog& dlg) {
  app.push_reentry();
  const int response = dlg.run();
  app.pop_reentry();
  return response;
}

bool unmodified_insert_key(GdkEventKey* event) {
  if (event == nullptr) {
    return false;
  }
  if (event->keyval != GDK_KEY_Insert && event->keyval != GDK_KEY_KP_Insert) {
    return false;
  }
  const guint mods =
      event->state & gtk_accelerator_get_default_mod_mask();
  return mods == 0;
}

bool insert_release_is_autorepeat(GdkEventKey* event) {
  if (event == nullptr || event->window == nullptr) {
    return false;
  }
  if (!GDK_IS_X11_WINDOW(event->window)) {
    return false;
  }
  Display* display = GDK_WINDOW_XDISPLAY(event->window);
  if (display == nullptr || XEventsQueued(display, QueuedAfterReading) == 0) {
    return false;
  }
  XEvent next;
  XPeekEvent(display, &next);
  return next.type == KeyPress &&
         next.xkey.keycode == event->hardware_keycode &&
         next.xkey.time == event->time;
}

}  // namespace

#ifdef LUNDUKE_EDIT_TEST_HOOKS
ssize_t (*MainWindow::test_write_hook_)(int, const void*, std::size_t,
                                        bool) = nullptr;
int (*MainWindow::test_dir_fsync_hook_)(int) = nullptr;
std::function<void(MainWindow*)> MainWindow::test_during_large_confirm_{};
std::function<const char*(MainWindow*)> MainWindow::test_discard_choice_{};
#endif

std::string MainWindow::ensure_save_as_path(std::string path) {
  // The file chooser already confirmed this path, including overwrite.
  // Rewriting the extension afterward writes a different file than the one
  // the user confirmed (notes.txt.txt must stay notes.txt.txt; "report" must
  // not become report.txt and clobber an existing file).
  return path;
}

bool MainWindow::parse_go_to_line(const std::string& text, int& line) {
  if (text.empty()) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed);
    if (consumed != text.size()) {
      return false;
    }
    line = value;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

MainWindow::MainWindow(Application& app) : app_(app) {
  set_title("Untitled — Lunduke Edit");
  set_default_size(640, 420);
  set_border_width(0);
  // Reinforce default icon for WMs that ignore gtk_window_set_default_icon_name.
  set_icon_name(resolve_app_icon_name());

  font_desc_ = Pango::FontDescription(
      app_.font().empty() ? "Monospace 11" : app_.font());
  tab_width_ = app_.tab_width();
  prefer_utf8_ = app_.prefer_utf8();
  open_charset_ = app_.open_charset().empty() ? "UTF-8" : app_.open_charset();
  // A fresh document is UTF-8 even when the next Open uses another charset.
  encoding_ = "UTF-8";
  saved_encoding_ = "UTF-8";

  doc_buffer_ = Gsv::Buffer::create();
  doc_buffer_->set_max_undo_levels(100);
  text_view_.set_buffer(doc_buffer_);
  auto buf = doc_buffer_;
  find_tag_ = buf->create_tag("lunduke-find-hit");
  find_tag_->property_background() = "#c4d8f0";
  long_hidden_tag_ = buf->create_tag("lunduke-long-line-hide");
  long_hidden_tag_->property_invisible() = true;

  // gtkmm's connect() defaults to after=true. insert-text and delete-range
  // run their default handlers first in that case, so "changed" updates the
  // status before the cache moves and delete-range has already removed the
  // text we need to measure. Run these before the default handlers.
  buf->signal_insert().connect(
      sigc::mem_fun(*this, &MainWindow::on_text_inserted), false);
  buf->signal_insert().connect(
      sigc::mem_fun(*this, &MainWindow::on_font_tag_inserted), true);
  buf->signal_erase().connect(
      sigc::mem_fun(*this, &MainWindow::on_text_erased), false);

  build_ui();
  build_menus();
  // Before the window is shown the CSS font is what the first layout uses.
  // apply_font also tags the buffer so a later Text → Font still changes
  // the line height after realize, which CSS alone does not.
  apply_font(font_desc_, app_.font_chosen());
  apply_tab_width(tab_width_);

  text_view_.set_wrap_mode(app_.wrap_text() ? Gtk::WRAP_WORD_CHAR
                                            : Gtk::WRAP_NONE);
  text_view_.set_accepts_tab(true);
  text_view_.set_left_margin(4);
  text_view_.set_right_margin(4);
  text_view_.set_top_margin(2);
  text_view_.set_bottom_margin(2);
  text_view_.set_show_line_marks(false);
  text_view_.set_auto_indent(false);
  text_view_.set_highlight_current_line(false);

  buf->signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_buffer_changed));
  buf->signal_modified_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_modified_changed));
  buf->signal_mark_set().connect(
      sigc::mem_fun(*this, &MainWindow::on_cursor_moved));
  // Undo/redo menu sensitivity must track GtkSourceView undo-manager state.
  // signal_changed() alone is unreliable (keyboard undo via View bindings,
  // and can-undo often notifies after changed). Use property notify + menu map.
  buf->property_can_undo().signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::update_undo_redo_sensitivity));
  buf->property_can_redo().signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::update_undo_redo_sensitivity));

  // The document starts as UTF-8. Only the "open next file" radios follow
  // the inherited charset, so a new window does not claim to be Latin-1.
  seeding_ = true;
  sync_encoding_radios();
  if (!prefer_utf8_ && open_latin1_item_) {
    open_latin1_item_->set_active(true);
  } else if (open_utf8_item_) {
    open_utf8_item_->set_active(true);
  }
  seeding_ = false;
  encoding_ = "UTF-8";
  saved_encoding_ = "UTF-8";

  property_is_active().signal_changed().connect([this]() {
    if (get_visible() && is_active()) {
      app_.note_window_focus(this);
    }
  });

  void (*paste_cb)(GtkTextView*, gpointer) =
      [](GtkTextView* view, gpointer user) {
        static_cast<MainWindow*>(user)->handle_paste_clipboard(view);
      };
  g_signal_connect(text_view_.gobj(), "paste-clipboard", G_CALLBACK(paste_cb),
                   this);
  text_view_.signal_button_press_event().connect(
      sigc::mem_fun(*this, &MainWindow::on_text_button_press), false);
  text_view_.signal_button_release_event().connect(
      sigc::mem_fun(*this, &MainWindow::on_text_button_release), false);
  buf->signal_begin_user_action().connect([this]() {
    in_user_action_ = true;
    ending_snapshotted_ = false;
  });
  buf->signal_end_user_action().connect([this]() {
    in_user_action_ = false;
    ending_snapshotted_ = false;
  });
  // Before the default handler mutates the buffer, remember which ending
  // snapshot undo/redo should restore. After it, install that snapshot.
  // Live insert/erase tracking is skipped while ending_restore_ is set.
  buf->signal_undo().connect(
      [this]() {
        // No snapshot: let insert/erase tracking follow the undo. Setting
        // ending_restore_ without a snapshot would skip that tracking and
        // then discard the per-line endings.
        if (!buffer() || !buffer()->can_undo() || source_lines_.empty() ||
            ending_undo_.empty()) {
          return;
        }
        ending_restore_ = true;
        ending_redo_.push_back(ending_kinds());
        pending_kinds_ = ending_undo_.back();
        ending_undo_.pop_back();
        have_pending_kinds_ = true;
      },
      false);
  buf->signal_undo().connect(
      [this]() {
        if (!ending_restore_) {
          return;
        }
        if (have_pending_kinds_) {
          apply_ending_kinds(pending_kinds_);
          have_pending_kinds_ = false;
        }
        ending_restore_ = false;
        if (auto live = text_view_.get_buffer()) {
          if (!source_lines_.empty() &&
              static_cast<int>(source_lines_.size()) != live->get_line_count()) {
            source_lines_.clear();
            clear_ending_history();
          }
        }
      },
      true);
  buf->signal_redo().connect(
      [this]() {
        if (!buffer() || !buffer()->can_redo() || source_lines_.empty() ||
            ending_redo_.empty()) {
          return;
        }
        ending_restore_ = true;
        ending_undo_.push_back(ending_kinds());
        pending_kinds_ = ending_redo_.back();
        ending_redo_.pop_back();
        have_pending_kinds_ = true;
      },
      false);
  buf->signal_redo().connect(
      [this]() {
        if (!ending_restore_) {
          return;
        }
        if (have_pending_kinds_) {
          apply_ending_kinds(pending_kinds_);
          have_pending_kinds_ = false;
        }
        ending_restore_ = false;
        if (auto live = text_view_.get_buffer()) {
          if (!source_lines_.empty() &&
              static_cast<int>(source_lines_.size()) != live->get_line_count()) {
            source_lines_.clear();
            clear_ending_history();
          }
        }
      },
      true);
  text_view_.signal_drag_data_received().connect(
      sigc::mem_fun(*this, &MainWindow::on_drag_data_received), false);
  // File drops reuse File → Open. Text drags keep the view's own targets.
  // Realize can replace the view's target list, so add the URI target again
  // once the widget is mapped.
  text_view_.drag_dest_add_uri_targets();
  text_view_.signal_map().connect([this]() {
    text_view_.drag_dest_add_uri_targets();
  });
  drag_dest_set(Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
  drag_dest_add_uri_targets();
  signal_drag_data_received().connect(
      sigc::mem_fun(*this, &MainWindow::on_window_drag_data_received), false);
  text_view_.property_overwrite().signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::sync_overwrite_status));

  show_all_children();
  status_find_frame_.hide();
  update_status();
  update_undo_redo_sensitivity();
  rebuild_recents_menu();

#ifdef LUNDUKE_EDIT_TEST_HOOKS
  // Command-line open test: report the real map, the status text, and
  // whether the busy cursor is on the window. Production builds omit this.
  if (g_getenv("LUNDUKE_EDIT_TEST_ARGV_CHILD") != nullptr) {
    signal_map().connect([this]() {
      unsigned long xid = 0;
      int watch = 0;
      if (auto win = get_window()) {
        xid = static_cast<unsigned long>(gdk_x11_window_get_xid(win->gobj()));
        auto cursor = win->get_cursor();
        if (cursor && cursor->get_cursor_type() == Gdk::WATCH) {
          watch = 1;
        }
      }
      if (!watch) {
        if (auto twin = text_view_.get_window(Gtk::TEXT_WINDOW_TEXT)) {
          auto cursor = twin->get_cursor();
          if (cursor && cursor->get_cursor_type() == Gdk::WATCH) {
            watch = 1;
          }
        }
      }
      // watch before status: the status text contains spaces.
      g_print("ARGV_MAPPED xid=%lu watch=%d status=%s\n", xid, watch,
              status_find_.get_text().c_str());
      fflush(stdout);
    });
  }
#endif
}

struct MainWindow::LoadState {
  MainWindow* window{nullptr};
  Glib::RefPtr<Gio::Cancellable> cancellable;
  Glib::RefPtr<Gio::File> file;
  Glib::RefPtr<Gio::FileInputStream> stream;
  std::string path;
  std::string raw;
  std::size_t max_bytes{0};
  std::size_t hard_bytes{0};
  bool allow_grow{false};
  bool cancel{false};
  bool close_after{false};
  bool success{false};
  bool active{false};
  bool mutated{false};
  bool undo_open{false};
  enum class Phase { Read, Insert };
  Phase phase{Phase::Read};
  bool decoded{false};
  bool inserted{false};
  Glib::ustring text;
  std::vector<char> kinds;
  NewlineStyle newlines{NewlineStyle::Lf};
  std::string encoding{"UTF-8"};
  std::string original;
  bool long_line{false};
  int insert_at{0};
  int expected_chars{0};
  Glib::ustring previous_text;
  bool previous_modified{false};
  std::string previous_encoding;
  NewlineStyle previous_newlines{NewlineStyle::Lf};
  sigc::connection idle;
  std::string base_name;
  int generation{0};
};

MainWindow::~MainWindow() {
  // Reattach without refreshing the gutter. Child widgets are still
  // alive here; a layout pass during teardown is not.
  if (view_parked_ && doc_buffer_) {
    text_view_.set_buffer(doc_buffer_);
    view_parked_ = false;
  }
  if (load_) {
    load_->window = nullptr;
    load_->cancel = true;
    load_->active = false;
    if (load_->cancellable) {
      load_->cancellable->cancel();
    }
    load_->idle.disconnect();
  }
  loading_ = false;
  find_idle_.disconnect();
  end_find_user_action();
}

Glib::RefPtr<Gsv::Buffer> MainWindow::buffer() {
  if (doc_buffer_) {
    return doc_buffer_;
  }
  return Glib::RefPtr<Gsv::Buffer>::cast_static(text_view_.get_buffer());
}

void MainWindow::park_document_view() {
  if (view_parked_ || !doc_buffer_) {
    return;
  }
  if (!scratch_buffer_) {
    scratch_buffer_ = Gtk::TextBuffer::create();
  } else if (scratch_buffer_->get_char_count() != 0) {
    scratch_buffer_->set_text("");
  }
  text_view_.set_buffer(scratch_buffer_);
  view_parked_ = true;
}

void MainWindow::unpark_document_view() {
  if (!view_parked_) {
    return;
  }
  if (doc_buffer_) {
    text_view_.set_buffer(doc_buffer_);
  }
  view_parked_ = false;
  if (gutter_) {
    gutter_->follow_view_adjustment();
    gutter_->refresh();
  }
}

void MainWindow::build_ui() {
  add(root_);
  root_.pack_start(menubar_, Gtk::PACK_SHRINK);

  gutter_ = Gtk::manage(new LineGutter(text_view_));
  editor_row_.pack_start(*gutter_, Gtk::PACK_SHRINK);

  scrolled_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
  scrolled_.add(text_view_);
  // Packing into the scrolled window replaces the text view's vadjustment.
  // Rebind the gutter to the adjustment that actually scrolls.
  gutter_->follow_view_adjustment();
  editor_row_.pack_start(scrolled_, Gtk::PACK_EXPAND_WIDGET);
  root_.pack_start(editor_row_, Gtk::PACK_EXPAND_WIDGET);

  status_box_.set_border_width(2);
  status_box_.set_spacing(2);

  status_pos_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_mode_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_enc_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_find_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_bytes_frame_.set_shadow_type(Gtk::SHADOW_IN);

  status_pos_.set_halign(Gtk::ALIGN_START);
  status_pos_.set_margin_start(6);
  status_pos_.set_margin_end(6);
  status_pos_.set_margin_top(2);
  status_pos_.set_margin_bottom(2);

  status_mode_.set_halign(Gtk::ALIGN_CENTER);
  status_mode_.set_margin_start(10);
  status_mode_.set_margin_end(10);
  status_mode_.set_margin_top(2);
  status_mode_.set_margin_bottom(2);

  status_enc_.set_halign(Gtk::ALIGN_CENTER);
  status_enc_.set_margin_start(8);
  status_enc_.set_margin_end(8);
  status_enc_.set_margin_top(2);
  status_enc_.set_margin_bottom(2);

  status_find_.set_halign(Gtk::ALIGN_CENTER);
  status_find_.set_margin_start(8);
  status_find_.set_margin_end(8);
  status_find_.set_margin_top(2);
  status_find_.set_margin_bottom(2);

  status_bytes_.set_halign(Gtk::ALIGN_END);
  status_bytes_.set_margin_start(6);
  status_bytes_.set_margin_end(6);
  status_bytes_.set_margin_top(2);
  status_bytes_.set_margin_bottom(2);

  status_pos_frame_.add(status_pos_);
  status_mode_frame_.add(status_mode_);
  status_enc_frame_.add(status_enc_);
  status_find_frame_.add(status_find_);
  status_bytes_frame_.add(status_bytes_);

  status_box_.pack_start(status_pos_frame_, Gtk::PACK_EXPAND_WIDGET);
  status_box_.pack_start(status_mode_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_enc_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_find_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_bytes_frame_, Gtk::PACK_SHRINK);

  root_.pack_start(status_box_, Gtk::PACK_SHRINK);
}

void MainWindow::build_menus() {
  add_accel_group(Gtk::AccelGroup::create());

  // ---- File ----
  auto* file_menu = Gtk::manage(new Gtk::Menu());
  auto* file_item = Gtk::manage(new Gtk::MenuItem("_File", true));
  file_item->set_submenu(*file_menu);
  menubar_.append(*file_item);

  auto* new_i = Gtk::manage(new Gtk::MenuItem("_New", true));
  new_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_new));
  new_i->add_accelerator("activate", get_accel_group(), GDK_KEY_n,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*new_i);

  auto* open_i = Gtk::manage(new Gtk::MenuItem("_Open…", true));
  open_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_open));
  open_i->add_accelerator("activate", get_accel_group(), GDK_KEY_o,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*open_i);

  auto* recent_item = Gtk::manage(new Gtk::MenuItem("Open _Recent", true));
  recents_menu_ = Gtk::manage(new Gtk::Menu());
  recent_item->set_submenu(*recents_menu_);
  file_menu->append(*recent_item);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* save_i = Gtk::manage(new Gtk::MenuItem("_Save", true));
  save_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_save));
  save_i->add_accelerator("activate", get_accel_group(), GDK_KEY_s,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*save_i);

  auto* save_as_i = Gtk::manage(new Gtk::MenuItem("Save _As…", true));
  save_as_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_save_as));
  save_as_i->add_accelerator("activate", get_accel_group(), GDK_KEY_s,
                             Gdk::CONTROL_MASK | Gdk::SHIFT_MASK,
                             Gtk::ACCEL_VISIBLE);
  file_menu->append(*save_as_i);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* page_setup_i = Gtk::manage(new Gtk::MenuItem("Page Set_up…", true));
  page_setup_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_page_setup));
  file_menu->append(*page_setup_i);

  auto* print_i = Gtk::manage(new Gtk::MenuItem("_Print…", true));
  print_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_print));
  print_i->add_accelerator("activate", get_accel_group(), GDK_KEY_p,
                           Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*print_i);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* exit_i = Gtk::manage(new Gtk::MenuItem("E_xit", true));
  exit_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_exit));
  exit_i->add_accelerator("activate", get_accel_group(), GDK_KEY_q,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*exit_i);

  // ---- Edit ----
  auto* edit_menu = Gtk::manage(new Gtk::Menu());
  auto* edit_item = Gtk::manage(new Gtk::MenuItem("_Edit", true));
  edit_item->set_submenu(*edit_menu);
  menubar_.append(*edit_item);
  // Refresh Undo/Redo when the Edit menu is opened so items always match
  // buffer->can_undo() / can_redo() (covers keyboard undo/redo paths).
  edit_menu->signal_map().connect(
      sigc::mem_fun(*this, &MainWindow::update_undo_redo_sensitivity));

  undo_item_ = Gtk::manage(new Gtk::MenuItem("_Undo", true));
  undo_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_undo));
  undo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_z,
                              Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*undo_item_);

  redo_item_ = Gtk::manage(new Gtk::MenuItem("_Redo", true));
  redo_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_redo));
  redo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_z,
                              Gdk::CONTROL_MASK | Gdk::SHIFT_MASK,
                              Gtk::ACCEL_VISIBLE);
  // Also Ctrl+Y
  redo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_y,
                              Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*redo_item_);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* cut_i = Gtk::manage(new Gtk::MenuItem("Cu_t", true));
  cut_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_cut));
  cut_i->add_accelerator("activate", get_accel_group(), GDK_KEY_x,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*cut_i);

  auto* copy_i = Gtk::manage(new Gtk::MenuItem("_Copy", true));
  copy_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_copy));
  copy_i->add_accelerator("activate", get_accel_group(), GDK_KEY_c,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*copy_i);

  auto* paste_i = Gtk::manage(new Gtk::MenuItem("_Paste", true));
  paste_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_paste));
  paste_i->add_accelerator("activate", get_accel_group(), GDK_KEY_v,
                           Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*paste_i);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* sel_i = Gtk::manage(new Gtk::MenuItem("Select _All", true));
  sel_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_select_all));
  sel_i->add_accelerator("activate", get_accel_group(), GDK_KEY_a,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*sel_i);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  // Notepad-like: Find under Edit (Search menu keeps a duplicate entry).
  auto* edit_find_i = Gtk::manage(new Gtk::MenuItem("_Find…", true));
  edit_find_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_find));
  edit_find_i->add_accelerator("activate", get_accel_group(), GDK_KEY_f,
                               Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*edit_find_i);

  // ---- Search ----
  auto* search_menu = Gtk::manage(new Gtk::Menu());
  auto* search_item = Gtk::manage(new Gtk::MenuItem("_Search", true));
  search_item->set_submenu(*search_menu);
  menubar_.append(*search_item);

  auto* find_i = Gtk::manage(new Gtk::MenuItem("_Find…", true));
  find_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_find));
  find_i->add_accelerator("activate", get_accel_group(), GDK_KEY_f,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  search_menu->append(*find_i);

  auto* find_next_i = Gtk::manage(new Gtk::MenuItem("Find _Next", true));
  find_next_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_find_next));
  find_next_i->add_accelerator("activate", get_accel_group(), GDK_KEY_F3,
                               Gdk::ModifierType(0), Gtk::ACCEL_VISIBLE);
  search_menu->append(*find_next_i);

  auto* goto_i = Gtk::manage(new Gtk::MenuItem("_Go to Line…", true));
  goto_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_go_to_line));
  goto_i->add_accelerator("activate", get_accel_group(), GDK_KEY_g,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  search_menu->append(*goto_i);

  // ---- Text ----
  auto* text_menu = Gtk::manage(new Gtk::Menu());
  auto* text_item = Gtk::manage(new Gtk::MenuItem("_Text", true));
  text_item->set_submenu(*text_menu);
  menubar_.append(*text_item);

  wrap_item_ = Gtk::manage(new Gtk::CheckMenuItem("_Wrap Text", true));
  wrap_item_->set_active(app_.wrap_text());
  wrap_item_->signal_toggled().connect(
      sigc::mem_fun(*this, &MainWindow::on_toggle_wrap));
  text_menu->append(*wrap_item_);

  line_numbers_item_ =
      Gtk::manage(new Gtk::CheckMenuItem("Show Line _Numbers", true));
  line_numbers_item_->set_active(true);
  line_numbers_item_->signal_toggled().connect(
      sigc::mem_fun(*this, &MainWindow::on_toggle_line_numbers));
  text_menu->append(*line_numbers_item_);

  auto* tab_i = Gtk::manage(new Gtk::MenuItem("_Tab Width…", true));
  tab_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_tab_width));
  text_menu->append(*tab_i);

  auto* font_under_text = Gtk::manage(new Gtk::MenuItem("_Font…", true));
  font_under_text->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_font));
  text_menu->append(*font_under_text);

  text_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  // Section titles are bold labels, not greyed commands. They do not activate.
  auto add_section = [](Gtk::Menu* menu, const char* title) {
    auto* item = Gtk::manage(new Gtk::MenuItem());
    auto* label = Gtk::manage(new Gtk::Label());
    label->set_markup(std::string("<b>") + title + "</b>");
    label->set_halign(Gtk::ALIGN_START);
    label->set_margin_start(8);
    label->set_margin_end(8);
    label->set_margin_top(2);
    item->add(*label);
    item->signal_select().connect([item]() { item->deselect(); });
    item->signal_button_press_event().connect(
        [](GdkEventButton*) { return true; });
    item->signal_activate().connect([]() {});
    menu->append(*item);
  };

  add_section(text_menu, "Encoding");

  Gtk::RadioMenuItem::Group enc_group;
  enc_utf8_item_ =
      Gtk::manage(new Gtk::RadioMenuItem(enc_group, "_UTF-8", true));
  enc_utf8_item_->set_active(true);
  enc_utf8_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_encoding_utf8));
  text_menu->append(*enc_utf8_item_);

  enc_latin1_item_ = Gtk::manage(
      new Gtk::RadioMenuItem(enc_group, "_Latin-1 (ISO-8859-1)", true));
  enc_latin1_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_encoding_latin1));
  text_menu->append(*enc_latin1_item_);

  text_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
  add_section(text_menu, "Open Next File As");

  Gtk::RadioMenuItem::Group open_group;
  // Distinct from the encoding radios, which already use U and L.
  open_utf8_item_ =
      Gtk::manage(new Gtk::RadioMenuItem(open_group, "Next file: UTF-_8", true));
  open_utf8_item_->set_active(true);
  open_utf8_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_open_pref_utf8));
  text_menu->append(*open_utf8_item_);

  open_latin1_item_ = Gtk::manage(new Gtk::RadioMenuItem(
      open_group, "Next file: Latin-_1", true));
  open_latin1_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_open_pref_latin1));
  text_menu->append(*open_latin1_item_);

  // ---- Help ----
  auto* help_menu = Gtk::manage(new Gtk::Menu());
  auto* help_item = Gtk::manage(new Gtk::MenuItem("_Help", true));
  help_item->set_submenu(*help_menu);
  menubar_.append(*help_item);

  auto* about_i = Gtk::manage(new Gtk::MenuItem("_About Lunduke Edit", true));
  about_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_about));
  help_menu->append(*about_i);
}

void MainWindow::clear_document_search_pins() {
  clear_selection_only_range();
  clear_extend_anchor();
  last_match_valid_ = false;
}

void MainWindow::load_seed_sample() {
  // Fresh window / first launch: blank untitled document (no demo text).
  clear_document_search_pins();
  seeding_ = true;
  auto buf = buffer();
  buf->begin_not_undoable_action();
  clear_find_highlights();
  buf->set_text("");
  buf->end_not_undoable_action();
  buf->set_modified(false);
  file_path_.clear();
  encoding_ = "UTF-8";
  saved_encoding_ = "UTF-8";
  encoding_dirty_ = false;
  newline_style_ = NewlineStyle::Lf;
  saved_newline_style_ = NewlineStyle::Lf;
  source_lines_.clear();
  clear_ending_history();
  loaded_bytes_.clear();
  loaded_text_.clear();
  loaded_bytes_valid_ = false;
  have_file_id_ = false;
  file_missing_ = false;
  file_unreadable_ = false;
  disk_error_noted_ = false;
  note_loaded_text("");
  set_dirty(false);
  seeding_ = false;
  long_line_present_ = false;
  long_window_line_ = -1;
  sync_encoding_radios();
  update_title();
  update_status();
  update_undo_redo_sensitivity();
  if (gutter_) {
    gutter_->refresh();
  }
  buf->place_cursor(buf->begin());
  maybe_restore_wrap();
  update_status();
}

bool MainWindow::confirm_large_open(const std::string& path) {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  if (test_mode()) {
    if (test_during_large_confirm_) {
      test_during_large_confirm_(this);
    }
    return test_large() != nullptr;
  }
#endif
  Gtk::MessageDialog dlg(
      *this,
      "This file is larger than 32 MiB.",
      false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text(
      "Opening \"" + path +
      "\" may use a lot of memory. Open it anyway?");
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Open", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_CANCEL);
  return run_modal(app_, dlg) == Gtk::RESPONSE_ACCEPT;
}

bool MainWindow::edits_path(const std::string& path) const {
  if (file_path_.empty() || path.empty()) {
    return false;
  }
  return canonical_path(file_path_) == canonical_path(path);
}

bool MainWindow::open_file(const std::string& path,
                           bool discard_already_confirmed) {
  if (opening_) {
    app_.defer_open(path);
    return false;
  }
  if (MainWindow* other = app_.find_window_editing(path)) {
    if (other != this) {
      other->present();
      // The discard prompt File → Open already showed was for this window.
      // The window that has the path still has to ask if its own buffer
      // is dirty, then re-read when the inode or mtime changed.
      return other->open_file(path, false);
    }
    present();
    opening_ = true;
    app_.push_reentry();
    const bool ok = reopen_same_path(path, discard_already_confirmed);
    opening_ = false;
    app_.pop_reentry();
    return ok;
  }
  opening_ = true;
  app_.push_reentry();
  const bool ok = open_file_body(path);
  opening_ = false;
  app_.pop_reentry();
  return ok;
}

bool MainWindow::reopen_same_path(const std::string& path,
                                  bool discard_already_confirmed) {
  const DiskCheck disk = file_on_disk(path, file_path_, have_file_id_, file_dev_,
                                      file_ino_, file_mtime_sec_,
                                      file_mtime_nsec_);
  if (disk.kind == DiskIdentity::Error) {
    // The buffer stays. Say why once: the close question may already have
    // named this failure, and Save must not open another dialog for it.
    last_open_error_ = disk.message;
    file_unreadable_ = true;
    set_dirty(true);
    if (!disk_error_noted_) {
      report_error("Could not check the file on disk.", disk.message);
      disk_error_noted_ = true;
    }
    return false;
  }
  if (disk.kind == DiskIdentity::Missing) {
    // Re-reading cannot succeed. Keep the buffer and mark it so Close asks.
    file_missing_ = true;
    file_unreadable_ = false;
    disk_error_noted_ = false;
    set_dirty(true);
    last_open_error_ = disk.message;
    report_error("This file was deleted.", path);
    return false;
  }
  const bool changed = disk.kind == DiskIdentity::Changed;
  // Unchanged on disk and the buffer is the loaded text: just show it.
  if (!changed && !dirty_) {
    return true;
  }
  // A dirty buffer is replaced only after the unsaved-changes prompt.
  // File → Open already asked; don't ask a second time.
  if (dirty_ && !discard_already_confirmed) {
    if (!changed) {
      return true;
    }
    if (!confirm_discard_or_save()) {
      return false;
    }
    // Save wrote this buffer. If that brought the file back in sync, stop.
    // A failed save returns false from confirm_discard_or_save.
    const DiskCheck again =
        file_on_disk(path, file_path_, have_file_id_, file_dev_, file_ino_,
                     file_mtime_sec_, file_mtime_nsec_);
    if (!dirty_ && again.kind == DiskIdentity::Unchanged) {
      return true;
    }
  }
  if (!changed && !dirty_) {
    return true;
  }
  return open_file_body(path);
}

bool MainWindow::open_file_body(const std::string& path) {
  if (!start_async_load(path)) {
    return false;
  }
  pump_async_load();
  const bool ok = load_ && load_->success;
  const bool close_after = load_ && load_->close_after;
  end_load_chrome();
  loading_ = false;
  if (load_) {
    load_->active = false;
    load_->window = nullptr;
    if (load_->cancellable) {
      load_->cancellable->cancel();
    }
  }
  load_.reset();
  if (close_after) {
    on_delete_event(nullptr);
  }
  return ok;
}

void MainWindow::begin_load_chrome(const std::string& path) {
  load_saved_editable_ = text_view_.get_editable();
  text_view_.set_editable(false);
  // Status first, then the cursor, then map. present() can map
  // synchronously, and the map handler must already see Opening.
  const std::string base = Glib::path_get_basename(path);
  status_find_.set_text(Glib::ustring("Opening ") + base + "…");
  status_find_frame_.show();
  if (!get_realized()) {
    realize();
  }
  auto display = get_display();
  if (!display) {
    display = Gdk::Display::get_default();
  }
  if (display && !load_watch_) {
    load_watch_ = Gdk::Cursor::create(display, Gdk::WATCH);
  }
  if (load_watch_) {
    if (auto win = get_window()) {
      win->set_cursor(load_watch_);
    }
    if (auto twin = text_view_.get_window(Gtk::TEXT_WINDOW_TEXT)) {
      twin->set_cursor(load_watch_);
    }
  }
  // Every open path (argv, Ctrl+O, Open Recent, drag-and-drop,
  // GApplication open) comes through here. Map before the read so a
  // slow CPU shows the window while the file is still loading.
  present();
}

void MainWindow::end_load_chrome() {
  unpark_document_view();
  text_view_.set_editable(load_saved_editable_);
  if (auto win = get_window()) {
    win->set_cursor(Glib::RefPtr<Gdk::Cursor>());
  }
  if (auto twin = text_view_.get_window(Gtk::TEXT_WINDOW_TEXT)) {
    twin->set_cursor(Glib::RefPtr<Gdk::Cursor>());
  }
  if (status_find_.get_text().find("Opening") == 0) {
    set_find_count(-1, false);
  }
}

void MainWindow::update_load_status() {
  if (!load_) {
    return;
  }
  Glib::ustring text = "Opening " + load_->base_name + "…";
  if (load_->phase == LoadState::Phase::Read && !load_->raw.empty()) {
    text += " " + format_bytes(load_->raw.size());
  } else if (load_->phase == LoadState::Phase::Insert &&
             load_->expected_chars > 0) {
    const int pct =
        (load_->insert_at * 100) / std::max(1, load_->expected_chars);
    text += " " + std::to_string(pct) + "%";
  }
  status_find_.set_text(text);
  status_find_frame_.show();
}

void MainWindow::pump_async_load() {
  while (load_ && load_->active) {
    g_main_context_iteration(nullptr, TRUE);
  }
}

void MainWindow::fail_async_load(const std::shared_ptr<LoadState>& state,
                                 const std::string& primary,
                                 const std::string& secondary) {
  if (!state || state->window != this) {
    return;
  }
  state->idle.disconnect();
  state->active = false;
  state->success = false;
  if (state->undo_open) {
    try {
      if (auto buf = buffer()) {
        buf->end_not_undoable_action();
      }
    } catch (...) {
    }
    state->undo_open = false;
  }
  if (state->mutated) {
    restore_buffer_after_failed_load(state->previous_text,
                                     state->previous_modified,
                                     state->previous_encoding,
                                     state->previous_newlines);
    state->mutated = false;
  }
  seeding_ = false;
  if (!primary.empty()) {
    if (primary == "Could not open file." ||
        primary == "File is too large to open.") {
      last_open_error_ = secondary.empty() ? state->path : secondary;
    } else if (primary.find("null byte") != std::string::npos) {
      last_open_error_ = primary;
    }
    report_error(primary, secondary.empty() ? state->path : secondary);
  }
}

void MainWindow::abort_async_load(const std::shared_ptr<LoadState>& state) {
  if (!state || state->window != this) {
    return;
  }
  state->idle.disconnect();
  state->cancel = true;
  state->active = false;
  state->success = false;
  if (state->undo_open) {
    try {
      if (auto buf = buffer()) {
        buf->end_not_undoable_action();
      }
    } catch (...) {
    }
    state->undo_open = false;
  }
  if (state->mutated) {
    restore_buffer_after_failed_load(state->previous_text,
                                     state->previous_modified,
                                     state->previous_encoding,
                                     state->previous_newlines);
    state->mutated = false;
  }
  seeding_ = false;
}

void MainWindow::finish_async_load(const std::shared_ptr<LoadState>& state) {
  if (!state) {
    return;
  }
  state->idle.disconnect();
  state->active = false;
  state->success = true;
}

void MainWindow::schedule_load_read(const std::shared_ptr<LoadState>& state) {
  if (!state || !state->stream || state->cancel || state->window != this) {
    abort_async_load(state);
    return;
  }
  const int gen = state->generation;
  state->stream->read_bytes_async(
      kLoadReadBytes,
      [state, gen](const Glib::RefPtr<Gio::AsyncResult>& result) {
        if (!state->window || state->generation != gen) {
          try {
            if (state->stream) {
              state->stream->read_bytes_finish(result);
            }
          } catch (...) {
          }
          return;
        }
        state->window->on_load_chunk(state, result);
      },
      state->cancellable, Glib::PRIORITY_DEFAULT);
}

void MainWindow::begin_load_stream(const std::shared_ptr<LoadState>& state) {
  const int gen = state->generation;
  state->file->read_async(
      [state, gen](const Glib::RefPtr<Gio::AsyncResult>& result) {
        if (!state->window || state->generation != gen) {
          try {
            state->file->read_finish(result);
          } catch (...) {
          }
          return;
        }
        state->window->on_load_opened(state, result);
      },
      state->cancellable, Glib::PRIORITY_DEFAULT);
}

bool MainWindow::start_async_load(const std::string& path) {
  const std::size_t cap = max_open_bytes();
  const std::size_t hard = max_open_hard_bytes();
  struct stat size_st {};
  if (::stat(path.c_str(), &size_st) == 0 && S_ISREG(size_st.st_mode) &&
      size_st.st_size > 0 &&
      static_cast<std::size_t>(size_st.st_size) > hard) {
    last_open_error_ = hard_open_refusal(hard);
    report_error("File is too large to open.", last_open_error_);
    return false;
  }

  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    last_open_error_ = open_failure_text(path, errno, false, false);
    report_error("Could not open file.", last_open_error_);
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    const int err = errno;
    ::close(fd);
    last_open_error_ = open_failure_text(path, err, false, false);
    report_error("Could not open file.", last_open_error_);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    const bool directory = S_ISDIR(st.st_mode);
    ::close(fd);
    last_open_error_ =
        open_failure_text(path, directory ? EISDIR : 0, true, directory);
    report_error("Could not open file.", last_open_error_);
    return false;
  }
  ::close(fd);

  std::size_t limit = cap;
  bool allow_grow = true;
  if (st.st_size > 0 && static_cast<std::size_t>(st.st_size) > cap) {
    if (!confirm_large_open(path)) {
      return false;
    }
    limit = hard;
    allow_grow = false;
  }

  auto state = std::make_shared<LoadState>();
  state->window = this;
  state->cancellable = Gio::Cancellable::create();
  state->file = Gio::File::create_for_path(path);
  state->path = path;
  state->max_bytes = limit;
  state->hard_bytes = hard;
  state->allow_grow = allow_grow;
  state->active = true;
  state->base_name = Glib::path_get_basename(path);
  try {
    if (st.st_size > 0) {
      auto hint = static_cast<std::size_t>(st.st_size);
      if (hint > limit) {
        hint = limit;
      }
      state->raw.reserve(hint);
    }
  } catch (const std::bad_alloc&) {
    report_error("Not enough memory to open this file.", path);
    return false;
  }

  load_ = state;
  loading_ = true;
  begin_load_chrome(path);
  begin_load_stream(state);
  return true;
}

void MainWindow::on_load_opened(const std::shared_ptr<LoadState>& state,
                                const Glib::RefPtr<Gio::AsyncResult>& result) {
  if (!state || state->window != this) {
    return;
  }
  try {
    state->stream = state->file->read_finish(result);
  } catch (const Gio::Error& err) {
    if (err.code() == Gio::Error::CANCELLED || state->cancel) {
      abort_async_load(state);
      return;
    }
    fail_async_load(state, "Could not open file.", err.what());
    return;
  } catch (const Glib::Error& err) {
    if (state->cancel) {
      abort_async_load(state);
      return;
    }
    fail_async_load(state, "Could not open file.", err.what());
    return;
  }
  if (state->cancel) {
    abort_async_load(state);
    return;
  }
  schedule_load_read(state);
}

void MainWindow::on_load_chunk(const std::shared_ptr<LoadState>& state,
                               const Glib::RefPtr<Gio::AsyncResult>& result) {
  if (!state || state->window != this) {
    return;
  }
  Glib::RefPtr<Glib::Bytes> bytes;
  try {
    bytes = state->stream->read_bytes_finish(result);
  } catch (const Gio::Error& err) {
    if (err.code() == Gio::Error::CANCELLED || state->cancel) {
      abort_async_load(state);
      return;
    }
    fail_async_load(state, "Could not open file.", err.what());
    return;
  } catch (const Glib::Error& err) {
    if (state->cancel) {
      abort_async_load(state);
      return;
    }
    fail_async_load(state, "Could not open file.", err.what());
    return;
  }
  if (state->cancel) {
    abort_async_load(state);
    return;
  }
  gsize n = 0;
  const auto* data =
      bytes ? static_cast<const char*>(bytes->get_data(n)) : nullptr;
  if (n == 0 || data == nullptr) {
    state->phase = LoadState::Phase::Insert;
    state->idle = Glib::signal_idle().connect([state]() {
      if (!state->window) {
        return false;
      }
      return state->window->on_load_idle(state);
    });
    return;
  }
  if (n > state->max_bytes || state->raw.size() > state->max_bytes - n) {
    if (state->allow_grow) {
      state->raw.clear();
      state->raw.shrink_to_fit();
      if (!confirm_large_open(state->path)) {
        abort_async_load(state);
        return;
      }
      state->allow_grow = false;
      state->max_bytes = state->hard_bytes;
      ++state->generation;
      if (state->cancellable) {
        state->cancellable->cancel();
      }
      state->cancellable = Gio::Cancellable::create();
      state->stream.reset();
      try {
        state->raw.reserve(std::min(state->hard_bytes, state->max_bytes));
      } catch (const std::bad_alloc&) {
        fail_async_load(state, "Not enough memory to open this file.",
                        state->path);
        return;
      }
      begin_load_stream(state);
      return;
    }
    state->raw.clear();
    fail_async_load(state, "File is too large to open.",
                    hard_open_refusal(state->hard_bytes));
    return;
  }
  try {
    state->raw.append(data, n);
  } catch (const std::bad_alloc&) {
    state->raw.clear();
    fail_async_load(state, "Not enough memory to open this file.", state->path);
    return;
  }
  update_load_status();
  schedule_load_read(state);
}

bool MainWindow::on_load_idle(const std::shared_ptr<LoadState>& state) {
  if (!state || state->window != this || !state->active) {
    return false;
  }
  if (state->cancel) {
    abort_async_load(state);
    return false;
  }
  if (!state->decoded) {
    try {
      state->original = state->raw;
      state->newlines = normalize_newlines(state->raw, state->kinds);
      if (prefer_utf8_) {
        if (g_utf8_validate(state->raw.data(),
                            static_cast<gssize>(state->raw.size()), nullptr)) {
          state->text = Glib::ustring(state->raw);
          state->encoding = "UTF-8";
        } else {
          state->text = Glib::convert(state->raw, "UTF-8", "ISO-8859-1");
          state->encoding = "ISO-8859-1";
        }
      } else {
        const std::string charset =
            open_charset_.empty() ? "ISO-8859-1" : open_charset_;
        state->text = Glib::convert(state->raw, "UTF-8", charset);
        state->encoding = charset;
      }
    } catch (const Glib::ConvertError& err) {
      fail_async_load(state, "Encoding error while opening.", err.what());
      return false;
    } catch (const std::bad_alloc&) {
      fail_async_load(state, "Not enough memory to open this file.",
                      state->path);
      return false;
    }
    state->raw.clear();
    state->raw.shrink_to_fit();
    state->long_line =
        has_long_line(state->text.data(), state->text.bytes(), kLongLineChars);
    if (bytes_contain_nul(state->original) || text_contains_nul(state->text)) {
      const std::string why =
          "This file contains a null byte and cannot be opened as text.";
      fail_async_load(state, why, state->path);
      return false;
    }
    auto buf = buffer();
    state->previous_text = buf->get_text();
    state->previous_modified = buf->get_modified();
    state->previous_encoding = encoding_;
    state->previous_newlines = newline_style_;
    state->expected_chars = static_cast<int>(state->text.length());
    state->decoded = true;
    state->phase = LoadState::Phase::Insert;
    update_load_status();
    // The window is already mapped. Return so expose and Escape run
    // before the buffer replace.
    return true;
  }

  if (!state->inserted) {
    auto buf = buffer();
    seeding_ = true;
    encoding_ = state->encoding;
    newline_style_ = state->newlines;
    long_line_present_ = false;
    long_window_line_ = -1;
    long_window_begin_ = 0;
    long_window_end_ = 0;
    clear_document_search_pins();
    clear_find_highlights();
    // Detach before set_text. Chunked insert on an attached view made
    // gtk_text_layout_validate shape every line on the main thread, so
    // argv open never mapped a window on a slow CPU.
    park_document_view();
    state->mutated = true;
    try {
      buf->begin_not_undoable_action();
      state->undo_open = true;
      buf->set_text(state->text);
    } catch (const std::bad_alloc&) {
      fail_async_load(state, "Not enough memory to open this file.",
                      state->path);
      return false;
    }
    state->inserted = true;
    if (buf->get_char_count() != state->expected_chars) {
      const std::string why =
          "This file contains a null byte and cannot be opened as text.";
      fail_async_load(state, why, state->path);
      return false;
    }
    update_load_status();
    // Let Escape land before commit. set_text does not return to the
    // main loop on its own.
    return true;
  }

  if (buffer() && buffer()->get_char_count() != state->expected_chars) {
    const std::string why =
        "This file contains a null byte and cannot be opened as text.";
    fail_async_load(state, why, state->path);
    return false;
  }
  if (!commit_loaded_text(state)) {
    fail_async_load(state, "Not enough memory to open this file.", state->path);
    return false;
  }
  finish_async_load(state);
  return false;
}

bool MainWindow::commit_loaded_text(const std::shared_ptr<LoadState>& state) {
  auto buf = buffer();
  if (!buf || !state) {
    return false;
  }
  try {
    note_loaded_text(state->text);
    buf->set_modified(false);
    file_path_ = state->path;
    saved_encoding_ = encoding_;
    encoding_dirty_ = false;
    saved_newline_style_ = state->newlines;
    loaded_bytes_ = std::move(state->original);
    loaded_text_ = state->text;
    loaded_bytes_valid_ = true;
    remember_source_lines(state->text, state->kinds);
    state->text.clear();
    clear_ending_history();
    remember_file_identity(state->path);
    file_missing_ = false;
    file_unreadable_ = false;
    disk_error_noted_ = false;
    set_dirty(false);
    if (state->undo_open) {
      buf->end_not_undoable_action();
      state->undo_open = false;
    }
    seeding_ = false;
    long_line_present_ = state->long_line;
    apply_editor_font_tag();
    sync_encoding_radios();
    buf->place_cursor(buf->begin());
    if (state->long_line) {
      force_wrap_off();
    } else {
      maybe_restore_wrap();
    }
    sync_long_line_window();
    update_title();
    update_status();
    update_undo_redo_sensitivity();
    if (gutter_) {
      gutter_->refresh();
    }
    remember_recent(state->path);
  } catch (const std::bad_alloc&) {
    return false;
  }
  return true;
}


void MainWindow::restore_buffer_after_failed_load(
    const Glib::ustring& previous_text, bool previous_modified,
    const std::string& previous_encoding, NewlineStyle previous_newlines) {
  auto buf = buffer();
  seeding_ = true;
  try {
    if (buf) {
      buf->begin_not_undoable_action();
      buf->set_text(previous_text);
      buf->end_not_undoable_action();
      buf->set_modified(previous_modified);
    }
  } catch (...) {
    try {
      if (buf) {
        buf->end_not_undoable_action();
      }
    } catch (...) {
    }
  }
  encoding_ = previous_encoding;
  newline_style_ = previous_newlines;
  note_loaded_text(previous_text);
  seeding_ = false;
  long_line_present_ =
      has_long_line(previous_text.data(), previous_text.bytes(), kLongLineChars);
  long_window_line_ = -1;
  apply_editor_font_tag();
  sync_long_line_window();
}

Glib::ustring MainWindow::current_basename() const {
  if (file_path_.empty()) {
    return "Untitled";
  }
  const auto pos = file_path_.find_last_of('/');
  if (pos == std::string::npos) {
    return file_path_;
  }
  return file_path_.substr(pos + 1);
}

void MainWindow::update_title() {
  Glib::ustring title = current_basename();
  if (dirty_) {
    title += " *";
  }
  title += " — Lunduke Edit";
  set_title(title);
}

void MainWindow::set_dirty(bool dirty) {
  dirty_ = dirty;
  update_title();
}

void MainWindow::update_cursor_status() {
  auto buf = buffer();
  if (!buf) {
    return;
  }
  auto iter = buf->get_iter_at_mark(buf->get_insert());
  const int line = iter.get_line() + 1;
  const int col = display_column_at(iter);
  status_pos_.set_text("Ln " + std::to_string(line) + ", Col " +
                       std::to_string(col));
}

void MainWindow::sync_overwrite_status() {
  overwrite_ = text_view_.get_overwrite();
  status_mode_.set_text(overwrite_ ? "Overwrite" : "Insert");
}

void MainWindow::update_bytes_status() {
  status_bytes_.set_text(format_bytes(cached_save_bytes()));
  sync_overwrite_status();
  status_enc_.set_text(encoding_ == "ISO-8859-1" ? "Latin-1" : encoding_);
}

void MainWindow::update_status() {
  update_cursor_status();
  update_bytes_status();
}

std::size_t MainWindow::cached_save_bytes() const {
  std::size_t n = 0;
  if (encoding_ == "UTF-8") {
    n = utf8_bytes_;
  } else if (auto buf = text_view_.get_buffer()) {
    const int chars = buf->get_char_count();
    if (chars > 0) {
      n = static_cast<std::size_t>(chars);
    }
  }
  // Each CRLF line adds a CR that the buffer's LF does not store. Mixed
  // files only add that byte for lines that actually use CRLF. A document
  // with no per-line record still follows the single dominant style.
  if (!source_lines_.empty()) {
    for (const auto& line : source_lines_) {
      if (line.kind == 'c') {
        ++n;
      }
    }
    return n;
  }
  if (newline_style_ == NewlineStyle::Crlf) {
    n += newline_count_;
  }
  return n;
}

void MainWindow::note_loaded_text(const Glib::ustring& text) {
  utf8_bytes_ = text.bytes();
  newline_count_ = count_newlines(text.data(), text.bytes());
}

void MainWindow::on_text_inserted(const Gtk::TextBuffer::iterator& pos,
                                  const Glib::ustring& text, int /*bytes*/) {
  if (!accepting_cr_ && text.find('\r') != Glib::ustring::npos) {
    const int offset = pos.get_offset();
    const Glib::ustring cleaned = strip_carriage_returns(text);
    g_signal_stop_emission_by_name(buffer()->gobj(), "insert-text");
    accepting_cr_ = true;
    buffer()->insert(buffer()->get_iter_at_offset(offset), cleaned);
    accepting_cr_ = false;
    return;
  }
  track_inserted_endings(pos, text);
  utf8_bytes_ += text.bytes();
  newline_count_ += count_newlines(text.data(), text.bytes());
  // get_chars_in_line walks the whole line. A bulk load already knows
  // whether any line is long; walking it here is what made a multi-megabyte
  // line take minutes.
  if (seeding_) {
    return;
  }
  if (has_long_line(text.data(), text.bytes(), kLongLineChars) ||
      pos.get_chars_in_line() >= kLongLineChars) {
    note_line_length(kLongLineChars);
    if (text_view_.get_wrap_mode() != Gtk::WRAP_NONE) {
      force_wrap_off();
    }
  }
}

void MainWindow::on_text_erased(const Gtk::TextBuffer::iterator& start,
                                const Gtk::TextBuffer::iterator& end) {
  track_erased_endings(start, end);
  const Glib::ustring gone = start.get_text(end);
  const std::size_t bytes = gone.bytes();
  const std::size_t newlines = count_newlines(gone.data(), bytes);
  utf8_bytes_ = (bytes > utf8_bytes_) ? 0 : utf8_bytes_ - bytes;
  newline_count_ =
      (newlines > newline_count_) ? 0 : newline_count_ - newlines;
}

void MainWindow::force_wrap_off() {
  if (text_view_.get_wrap_mode() == Gtk::WRAP_NONE &&
      (wrap_item_ == nullptr || !wrap_item_->get_active())) {
    return;
  }
  suppress_wrap_pref_ = true;
  text_view_.set_wrap_mode(Gtk::WRAP_NONE);
  if (wrap_item_ && wrap_item_->get_active()) {
    wrap_item_->set_active(false);
  }
  suppress_wrap_pref_ = false;
}

bool MainWindow::is_empty_untitled() const {
  if (dirty_ || !file_path_.empty()) {
    return false;
  }
  // The view shows a scratch buffer while a load is parked. Emptiness
  // follows the document, which is what Open and drag-and-drop reuse.
  if (doc_buffer_) {
    return doc_buffer_->get_char_count() == 0;
  }
  auto buf = text_view_.get_buffer();
  return buf && buf->get_char_count() == 0;
}

bool MainWindow::on_focus_in_event(GdkEventFocus* event) {
  app_.note_window_focus(this);
  return Gtk::ApplicationWindow::on_focus_in_event(event);
}

void MainWindow::update_undo_redo_sensitivity() {
  auto buf = buffer();
  if (undo_item_) {
    undo_item_->set_sensitive(buf && buf->can_undo());
  }
  if (redo_item_) {
    redo_item_->set_sensitive(buf && buf->can_redo());
  }
}

void MainWindow::on_buffer_changed() {
  // A bulk load replaces the buffer in one set_text. Layout, the long-line
  // tag, and the status line run once after that, from commit_loaded_text.
  if (seeding_) {
    return;
  }
  // Dirty state follows the undo save point (signal_modified_changed),
  // not every change. Undo back to the saved text must clear the marker.
  if (find_highlights_on_) {
    find_highlights_on_ = false;
    clear_find_highlights();
  }
  set_find_count(-1, false);
  update_bytes_status();
  sync_long_line_window();
  if (text_view_.get_wrap_mode() != Gtk::WRAP_NONE) {
    auto buf = text_view_.get_buffer();
    if (buf) {
      auto iter = buf->get_iter_at_mark(buf->get_insert());
      if (iter.get_chars_in_line() >= kLongLineChars) {
        force_wrap_off();
      }
    }
  }
  maybe_restore_wrap();
  if (gutter_) {
    gutter_->queue_draw();
  }
}

void MainWindow::on_modified_changed() { refresh_dirty_from_buffer(); }

void MainWindow::refresh_dirty_from_buffer() {
  if (seeding_) {
    return;
  }
  auto buf = buffer();
  const bool text_dirty = buf && buf->get_modified();
  set_dirty(text_dirty || encoding_dirty_ || file_missing_ || file_unreadable_);
}

void MainWindow::refresh_disk_flags() {
  file_missing_ = false;
  file_unreadable_ = false;
  if (file_path_.empty() || !have_file_id_) {
    disk_error_noted_ = false;
    refresh_dirty_from_buffer();
    return;
  }
  const DiskCheck disk =
      file_on_disk(file_path_, file_path_, have_file_id_, file_dev_, file_ino_,
                   file_mtime_sec_, file_mtime_nsec_);
  if (disk.kind == DiskIdentity::Missing) {
    file_missing_ = true;
    last_notice_ = disk.message;
    disk_error_noted_ = false;
  } else if (disk.kind == DiskIdentity::Error) {
    // Remember the strerror for the save question. Do not also open an
    // error dialog; Close would then show the same sentence twice.
    file_unreadable_ = true;
    last_notice_ = disk.message;
  } else {
    disk_error_noted_ = false;
  }
  refresh_dirty_from_buffer();
}

int MainWindow::display_column_at(const Gtk::TextIter& iter) const {
  const int line_off = iter.get_line_offset();
  // A multi-megabyte line must not be walked on every cursor motion.
  // Past the cap, the column is the character index (tabs are not expanded).
  if (line_off > kMaxColumnWalk) {
    return line_off + 1;
  }
  Gtk::TextIter line_start = iter;
  line_start.set_line_offset(0);
  const int tab = std::max(1, tab_width_);
  int col = 0;
  for (auto it = line_start; it.compare(iter) < 0; it.forward_char()) {
    if (it.get_char() == '\t') {
      col += tab - (col % tab);
    } else {
      col += 1;
    }
  }
  return col + 1;
}

void MainWindow::report_error(const Glib::ustring& primary,
                              const Glib::ustring& secondary) {
  ++error_reports_;
  last_error_primary_ = primary;
  last_error_secondary_ = secondary;
  if (test_mode()) {
    return;
  }
  Gtk::MessageDialog dlg(*this, primary, false, Gtk::MESSAGE_ERROR,
                         Gtk::BUTTONS_OK, true);
  dlg.set_secondary_text(secondary);
  run_modal(app_, dlg);
}

void MainWindow::on_cursor_moved(
    const Gtk::TextBuffer::iterator& /*loc*/,
    const Glib::RefPtr<Gtk::TextBuffer::Mark>& mark) {
  // set_text moves the cursor to the end of a just-loaded line. Walking
  // that line for the column, or retagging it, belongs after the load.
  if (seeding_) {
    return;
  }
  if (mark == buffer()->get_insert()) {
    update_cursor_status();
    sync_long_line_window();
  }
}

bool MainWindow::confirm_discard_or_save(DiscardKind kind) {
  if (kind == DiscardKind::Continue) {
    refresh_disk_flags();
  }
  const bool ask = dirty_ || file_missing_ || file_unreadable_;
  if (kind == DiscardKind::ReloadDisk) {
    if (!dirty_) {
      return true;
    }
  } else if (!ask) {
    return true;
  }

  Glib::ustring primary;
  Glib::ustring secondary;
  int default_response = Gtk::RESPONSE_ACCEPT;
  last_prompt_accept_ = "_Save";
  if (kind == DiscardKind::ReloadDisk) {
    // Don't Save is the focused button. Enter must load the disk copy.
    // Save is still available, and the text says it replaces the file
    // and cancels the reload.
    primary = "Save changes before reloading?";
    secondary =
        "Saving will replace the file on disk and cancel the reload. "
        "Don't Save drops the edits in this window and loads the copy "
        "on disk.";
    default_response = Gtk::RESPONSE_REJECT;
  } else if (file_missing_) {
    primary = "This file was deleted.";
    secondary = "\"" + current_basename() +
                "\" is no longer on disk. Save it again?";
    default_response = Gtk::RESPONSE_ACCEPT;
  } else if (file_unreadable_) {
    primary = "Could not check the file on disk.";
    // One dialog for this failure. The strerror is here, and so is the
    // way out: this path cannot be written, so Save As keeps the text.
    // Enter runs Save As. A plain Save cannot write this path.
    const Glib::ustring why =
        last_notice_.empty() ? Glib::ustring()
                             : Glib::ustring(last_notice_ + "\n");
    secondary = why +
                "This path cannot be written. Use Save As to keep the text.\n"
                "Save the text in this window before continuing?";
    disk_error_noted_ = true;
    default_response = kPromptSaveAs;
    last_prompt_accept_ = "Save _As";
  } else {
    primary = "Save changes before continuing?";
    secondary = "\"" + current_basename() + "\" has unsaved changes.";
    default_response = Gtk::RESPONSE_ACCEPT;
  }
  last_prompt_primary_ = primary;
  last_prompt_secondary_ = secondary;
  last_prompt_default_ = default_response;

  // Tests set this so a modal dialog does not block. Unset in normal use,
  // and compiled out of the production binary.
  const char* choice = nullptr;
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  if (test_discard_choice_) {
    choice = test_discard_choice_(this);
  }
#endif
  if (choice == nullptr) {
    choice = test_discard();
  }
  if (choice != nullptr) {
    if (std::strcmp(choice, "cancel") == 0) {
      return false;
    }
    if (std::strcmp(choice, "discard") == 0) {
      return true;
    }
    if (std::strcmp(choice, "save") == 0) {
      return save_document();
    }
    if (std::strcmp(choice, "save-as") == 0) {
      return save_as_dialog();
    }
  }
  Gtk::MessageDialog dlg(*this, primary, false, Gtk::MESSAGE_QUESTION,
                         Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text(secondary);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Don't Save", Gtk::RESPONSE_REJECT);
  if (file_unreadable_) {
    dlg.add_button("Save _As", kPromptSaveAs);
  } else {
    dlg.add_button("_Save", Gtk::RESPONSE_ACCEPT);
  }
  dlg.set_default_response(default_response);
  const int resp = run_modal(app_, dlg);
  if (resp == Gtk::RESPONSE_CANCEL || resp == Gtk::RESPONSE_DELETE_EVENT) {
    return false;
  }
  if (resp == kPromptSaveAs) {
    return save_as_dialog();
  }
  if (resp == Gtk::RESPONSE_ACCEPT) {
    return save_document();
  }
  return true;
}

void MainWindow::on_new() {
  if (!confirm_discard_or_save()) {
    return;
  }
  clear_document_search_pins();
  seeding_ = true;
  auto buf = buffer();
  buf->begin_not_undoable_action();
  clear_find_highlights();
  buf->set_text("");
  buf->end_not_undoable_action();
  buf->set_modified(false);
  file_path_.clear();
  encoding_ = "UTF-8";
  saved_encoding_ = "UTF-8";
  encoding_dirty_ = false;
  newline_style_ = NewlineStyle::Lf;
  saved_newline_style_ = NewlineStyle::Lf;
  source_lines_.clear();
  clear_ending_history();
  loaded_bytes_.clear();
  loaded_text_.clear();
  loaded_bytes_valid_ = false;
  have_file_id_ = false;
  file_missing_ = false;
  file_unreadable_ = false;
  disk_error_noted_ = false;
  note_loaded_text("");
  set_dirty(false);
  seeding_ = false;
  long_line_present_ = false;
  long_window_line_ = -1;
  apply_editor_font_tag();
  sync_encoding_radios();
  maybe_restore_wrap();
  update_title();
  update_status();
  update_undo_redo_sensitivity();
}

void MainWindow::on_open() {
  if (!confirm_discard_or_save()) {
    return;
  }
  Gtk::FileChooserDialog dlg(*this, "Open File",
                             Gtk::FILE_CHOOSER_ACTION_OPEN);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Open", Gtk::RESPONSE_ACCEPT);
  auto filter = Gtk::FileFilter::create();
  filter->set_name("Text files");
  filter->add_mime_type("text/plain");
  filter->add_pattern("*.txt");
  filter->add_pattern("*.md");
  filter->add_pattern("*");
  dlg.add_filter(filter);
  if (run_modal(app_, dlg) != Gtk::RESPONSE_ACCEPT) {
    return;
  }
  auto file = dlg.get_file();
  if (!file || !file->is_native() || file->get_path().empty()) {
    report_error("Cannot open this location.",
                 "Only local files can be opened.");
    return;
  }
  open_file(file->get_path(), true);
}

void MainWindow::on_open_recent(const std::string& path) {
  if (!confirm_discard_or_save()) {
    return;
  }
  open_file(path, true);
}

bool MainWindow::save_to_path(const std::string& path) {
  const bool same_loaded =
      !file_path_.empty() && canonical_path(path) == canonical_path(file_path_);
  DiskCheck disk;
  if (same_loaded) {
    disk = file_on_disk(path, file_path_, have_file_id_, file_dev_, file_ino_,
                        file_mtime_sec_, file_mtime_nsec_);
  }
  if (same_loaded && disk.kind == DiskIdentity::Error && !force_replace_) {
    // The close question already explained this. A direct Save says it
    // once. A second Save, or Save chosen from that question, stays quiet
    // and leaves Save As as the way to keep the text.
    last_save_error_ = disk.message +
                       " This path cannot be written. Use Save As to keep the text.";
    file_unreadable_ = true;
    set_dirty(true);
    if (!disk_error_noted_) {
      report_error(
          "This path cannot be written.",
          std::string("Use Save As to keep the text.\n") + disk.message);
      disk_error_noted_ = true;
    }
    return false;
  }
  if (same_loaded && disk.kind == DiskIdentity::Missing) {
    file_missing_ = true;
    set_dirty(true);
  }
  if (disk.kind == DiskIdentity::Changed && !force_replace_) {
    const DiskChangeChoice choice = confirm_file_changed(path);
    if (choice == DiskChangeChoice::Cancel) {
      return false;
    }
    if (choice == DiskChangeChoice::Reload) {
      // Don't Save is the default and re-reads the file. Save writes this
      // buffer (force_replace_ skips a second disk dialog) and does not
      // load the copy the user asked to reload. The question says so.
      if (dirty_) {
        force_replace_ = true;
        const bool proceed =
            confirm_discard_or_save(DiscardKind::ReloadDisk);
        force_replace_ = false;
        if (!proceed) {
          return false;
        }
        if (!dirty_) {
          last_notice_ =
              "Reload cancelled. The file on disk was replaced with the "
              "text in this window.";
          return true;
        }
      }
      open_file_body(path);
      return false;
    }
  }
  // A clean buffer whose encoding and newline style still match the load
  // is already the file. Rewriting would drop xattrs and bump mtime.
  // A deleted file is not "already the file": the buffer has to be written
  // back even when the text was not edited.
  if (!dirty_ && !file_missing_ && same_loaded &&
      disk.kind == DiskIdentity::Unchanged && encoding_ == saved_encoding_ &&
      newline_style_ == saved_newline_style_) {
    remember_recent(path);
    return true;
  }

  const Glib::ustring text = text_view_.get_buffer()->get_text();
  std::string out_bytes;
  try {
    const bool unchanged_text =
        loaded_bytes_valid_ && text == loaded_text_ &&
        encoding_ == saved_encoding_;
    if (unchanged_text) {
      out_bytes = loaded_bytes_;
    } else {
      std::string encoded;
      if (encoding_ == "UTF-8") {
        encoded.assign(text.data(), text.bytes());
      } else {
        encoded = Glib::convert(text, encoding_, "UTF-8");
      }
      if (source_lines_.empty()) {
        out_bytes = apply_newline_style(encoded, newline_style_);
      } else {
        std::vector<SavedLine> saved;
        saved.reserve(source_lines_.size());
        for (const auto& line : source_lines_) {
          saved.push_back(SavedLine{line.text, line.kind});
        }
        out_bytes = apply_line_endings(encoded, saved, newline_style_);
      }
    }
  } catch (const Glib::ConvertError& e) {
    report_error("Encoding error while saving.", e.what());
    return false;
  } catch (const std::bad_alloc&) {
    report_error("Not enough memory to save this file.", path);
    return false;
  }

  std::string write_error;
  if (!replace_file_contents(path, out_bytes, write_error)) {
    last_save_error_ = write_error.empty() ? path : write_error;
    report_error("Could not save file.", last_save_error_);
    return false;
  }
  file_path_ = path;
  saved_encoding_ = encoding_;
  encoding_dirty_ = false;
  saved_newline_style_ = newline_style_;
  loaded_bytes_ = out_bytes;
  loaded_text_ = text;
  loaded_bytes_valid_ = true;
  {
    std::string scan = out_bytes;
    std::vector<char> kinds;
    normalize_newlines(scan, kinds);
    remember_source_lines(text, kinds);
  }
  remember_file_identity(path);
  file_missing_ = false;
  file_unreadable_ = false;
  disk_error_noted_ = false;
  // Records an undo save point so undo/redo back to this text clears
  // the buffer's modified flag.
  buffer()->set_modified(false);
  set_dirty(false);
  update_status();
  remember_recent(path);
  return true;
}

bool MainWindow::save_document() {
  if (file_path_.empty()) {
    return save_as_dialog();
  }
  return save_to_path(file_path_);
}

void MainWindow::on_save() { save_document(); }

bool MainWindow::save_as_dialog() {
  if (const char* hook = test_save_as()) {
    if (std::strcmp(hook, "cancel") == 0) {
      return false;
    }
    if (hook[0] != '\0') {
      return save_to_path(hook);
    }
  }
  Gtk::FileChooserDialog dlg(*this, "Save As",
                             Gtk::FILE_CHOOSER_ACTION_SAVE);
  dlg.set_do_overwrite_confirmation(true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Save", Gtk::RESPONSE_ACCEPT);
  auto filter = Gtk::FileFilter::create();
  filter->set_name("Text files");
  filter->add_mime_type("text/plain");
  filter->add_pattern("*.txt");
  filter->add_pattern("*.md");
  filter->add_pattern("*");
  dlg.add_filter(filter);
  if (!file_path_.empty()) {
    dlg.set_filename(file_path_);
  } else {
    dlg.set_current_name("Untitled.txt");
  }
  if (run_modal(app_, dlg) != Gtk::RESPONSE_ACCEPT) {
    return false;
  }
  auto file = dlg.get_file();
  if (!file || !file->is_native() || file->get_path().empty()) {
    report_error("Cannot save to this location.",
                 "Only local files can be saved.");
    return false;
  }
  return save_to_path(ensure_save_as_path(file->get_path()));
}

void MainWindow::on_save_as() { save_as_dialog(); }

void MainWindow::on_exit() {
  if (!app_.confirm_quit()) {
    return;
  }
  // hide() leaves the process registered as org.lunduke.LundukeEdit.
  app_.quit();
}

bool MainWindow::on_delete_event(GdkEventAny* /*event*/) {
  // Closing during a load cancels the read and finishes the close after
  // the loader returns, so this window is not freed mid-open.
  if (loading_ && load_) {
    load_->cancel = true;
    load_->close_after = true;
    if (load_->cancellable) {
      load_->cancellable->cancel();
    }
    return true;
  }
  if (!confirm_discard_or_save()) {
    // Keep the window. GTK must not hide or destroy it.
    return true;
  }
  // Hide, then the application deletes the C++ window after this returns.
  hide();
  return true;
}

bool MainWindow::on_key_press_event(GdkEventKey* event) {
  if (loading_ && load_ && event != nullptr && event->keyval == GDK_KEY_Escape) {
    load_->cancel = true;
    if (load_->cancellable) {
      load_->cancellable->cancel();
    }
    return true;
  }
  if (unmodified_insert_key(event)) {
    // X11 auto-repeat delivers a synthetic release+press with one timestamp.
    // The release handler marks the following press so it does not toggle.
    if (swallow_insert_repeat_) {
      swallow_insert_repeat_ = false;
      return true;
    }
    text_view_.set_overwrite(!text_view_.get_overwrite());
    sync_overwrite_status();
    return true;
  }
  return Gtk::ApplicationWindow::on_key_press_event(event);
}

bool MainWindow::on_key_release_event(GdkEventKey* event) {
  if (unmodified_insert_key(event) && insert_release_is_autorepeat(event)) {
    swallow_insert_repeat_ = true;
    return true;
  }
  swallow_insert_repeat_ = false;
  return Gtk::ApplicationWindow::on_key_release_event(event);
}

void MainWindow::on_undo() {
  auto buf = buffer();
  if (buf && buf->can_undo()) {
    buf->undo();
    refresh_dirty_from_buffer();
    update_undo_redo_sensitivity();
    update_status();
  }
}

void MainWindow::on_redo() {
  auto buf = buffer();
  if (buf && buf->can_redo()) {
    buf->redo();
    refresh_dirty_from_buffer();
    update_undo_redo_sensitivity();
    update_status();
  }
}

void MainWindow::on_cut() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->cut_clipboard(clip);
  update_undo_redo_sensitivity();
}

void MainWindow::on_copy() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->copy_clipboard(clip);
}

bool MainWindow::read_clipboard_text(const Glib::RefPtr<Gtk::Clipboard>& clip,
                                     Glib::ustring& out) {
  out.clear();
  if (!clip) {
    return true;
  }
  const std::size_t cap = max_paste_bytes();
  try {
    // SelectionData length is checked before any Glib::ustring is built, so a
    // rejected paste does not allocate a second copy of the clipboard.
    Gtk::SelectionData data = clip->wait_for_contents("UTF8_STRING");
    if (data.get_length() <= 0) {
      data = clip->wait_for_contents("STRING");
    }
    const int len = data.get_length();
    if (len <= 0 || data.get_data() == nullptr) {
      return true;
    }
    if (static_cast<std::size_t>(len) > cap) {
      report_error("Paste is too large.",
                   "A single paste is limited to " + format_bytes(cap) + ".");
      return false;
    }
    const char* bytes = reinterpret_cast<const char*>(data.get_data());
    if (g_utf8_validate(bytes, len, nullptr)) {
      out = Glib::ustring(bytes, bytes + len);
    } else {
      out = Glib::convert(std::string(bytes, bytes + len), "UTF-8",
                          "ISO-8859-1");
    }
    return true;
  } catch (const std::bad_alloc&) {
    report_error("Paste is too large.",
                 "A single paste is limited to " + format_bytes(cap) + ".");
    return false;
  } catch (const Glib::ConvertError& e) {
    report_error("Could not paste.", e.what());
    return false;
  }
}

bool MainWindow::clipboard_paste_allowed() {
  Glib::ustring text;
  return read_clipboard_text(Gtk::Clipboard::get(), text);
}

void MainWindow::insert_pasted_text(const Glib::ustring& text) {
  if (text.empty()) {
    return;
  }
  auto buf = buffer();
  if (!buf) {
    return;
  }
  buf->begin_user_action();
  if (buf->get_has_selection()) {
    buf->erase_selection(true, text_view_.get_editable());
  }
  buf->insert_at_cursor(text);
  buf->end_user_action();
  update_undo_redo_sensitivity();
}

void MainWindow::insert_primary_paste(const Glib::ustring& text,
                                     GdkEventButton* event) {
  if (text.empty() || event == nullptr) {
    return;
  }
  auto buf = buffer();
  if (!buf) {
    return;
  }
  int x = static_cast<int>(event->x);
  int y = static_cast<int>(event->y);
  int bx = x;
  int by = y;
  GdkWindow* text_win = gtk_text_view_get_window(GTK_TEXT_VIEW(text_view_.gobj()),
                                                 GTK_TEXT_WINDOW_TEXT);
  if (text_win != nullptr && event->window == text_win) {
    text_view_.window_to_buffer_coords(Gtk::TEXT_WINDOW_TEXT, x, y, bx, by);
  } else if (event->window != nullptr) {
    text_view_.window_to_buffer_coords(Gtk::TEXT_WINDOW_WIDGET, x, y, bx, by);
  }
  Gtk::TextIter where;
  text_view_.get_iter_at_location(where, bx, by);

  bool inside = false;
  Gtk::TextIter sel_a, sel_b;
  const bool have_sel =
      buf->get_selection_bounds(sel_a, sel_b) && sel_a != sel_b;
  int sel_start = 0;
  int sel_end = 0;
  if (have_sel) {
    sel_start = sel_a.get_offset();
    sel_end = sel_b.get_offset();
    inside = where.compare(sel_a) > 0 && where.compare(sel_b) < 0;
  }
  const int insert_at = where.get_offset();
  const int added = static_cast<int>(text.length());

  buf->begin_user_action();
  if (inside) {
    // A click inside the selection replaces it. A click outside leaves the
    // selection in place and inserts at the pointer.
    buf->erase_selection(true, text_view_.get_editable());
    buf->insert_at_cursor(text);
  } else {
    buf->insert(where, text);
    // insert() moves the cursor to the pasted text and drops the highlight.
    // Put the selection back on the original span.
    if (have_sel) {
      int a = sel_start;
      int b = sel_end;
      if (insert_at <= sel_start) {
        a += added;
        b += added;
      }
      buf->select_range(buf->get_iter_at_offset(a), buf->get_iter_at_offset(b));
    }
  }
  buf->end_user_action();
  update_undo_redo_sensitivity();
}

void MainWindow::handle_paste_clipboard(GtkTextView* view) {
  // Stop the default handler. It would fetch the clipboard again, so a
  // clipboard owner could swap in a larger payload after the size check.
  g_signal_stop_emission_by_name(view, "paste-clipboard");
  app_.push_reentry();
  Glib::ustring text;
  const bool ok = read_clipboard_text(Gtk::Clipboard::get(), text);
  if (ok) {
    insert_pasted_text(text);
  }
  app_.pop_reentry();
}

bool MainWindow::on_text_button_press(GdkEventButton* event) {
  if (event == nullptr || event->button != 2) {
    return false;
  }
  // A double-click is press, release, press, 2BUTTON_PRESS, release.
  // A triple-click adds another press and 3BUTTON_PRESS. Only the first
  // press inserts. Later presses in the same gesture do not, including a
  // third press that is still within the double-click time of the second.
  if (event->type != GDK_BUTTON_PRESS) {
    return true;
  }
  guint double_time = 400;
  if (auto settings = Gtk::Settings::get_default()) {
    const int configured = settings->property_gtk_double_click_time();
    if (configured > 0) {
      double_time = static_cast<guint>(configured);
    }
  }
  const bool same_gesture = last_middle_paste_time_ != 0 && event->time != 0 &&
                            event->time - last_middle_paste_time_ <= double_time;
  // Remember this press even when it does not insert, so the next press
  // in a triple-click is measured from it. Time 0 is the test seam that
  // pastes on every press.
  if (event->time != 0) {
    last_middle_paste_time_ = event->time;
  }
  if (same_gesture) {
    return true;
  }
  // GTK pastes the primary selection on middle-button press, at the click.
  // Consume the press so that gesture does not run, then insert once at the
  // pointer after the size check.
  app_.push_reentry();
  Glib::ustring text;
  const bool ok = read_clipboard_text(
      Gtk::Clipboard::get(GDK_SELECTION_PRIMARY), text);
  if (ok) {
    insert_primary_paste(text, event);
  }
  app_.pop_reentry();
  return true;
}

bool MainWindow::on_text_button_release(GdkEventButton* event) {
  // The paste already happened on press. Swallow the release so a second
  // handler cannot insert again.
  if (event == nullptr || event->button != 2) {
    return false;
  }
  return true;
}

namespace {

bool selection_is_uri_list(const Gtk::SelectionData& data) {
  if (data.get_length() <= 0) {
    return false;
  }
  if (data.targets_include_uri()) {
    return true;
  }
  const std::string type = data.get_data_type();
  return type == "text/uri-list";
}

}  // namespace

void MainWindow::open_dropped_uris(const std::vector<Glib::ustring>& uris) {
  if (dropping_uris_ || uris.empty()) {
    return;
  }
  dropping_uris_ = true;
  struct Clear {
    bool& flag;
    ~Clear() { flag = false; }
  } clear{dropping_uris_};

  std::vector<std::string> paths;
  bool remote = false;
  for (const auto& uri : uris) {
    auto file = Gio::File::create_for_uri(uri);
    if (!file || !file->is_native() || file->get_path().empty()) {
      remote = true;
      continue;
    }
    paths.push_back(file->get_path());
  }
  if (remote) {
    report_error("Cannot open this location.", "Only local files can be opened.");
  }
  if (paths.empty()) {
    return;
  }
  if (!confirm_discard_or_save()) {
    return;
  }
  open_file(paths.front(), true);
  if (paths.size() > 1) {
    app_.open_documents(
        std::vector<std::string>(paths.begin() + 1, paths.end()));
  }
}

void MainWindow::on_drag_data_received(
    const Glib::RefPtr<Gdk::DragContext>& context, int /*x*/, int /*y*/,
    const Gtk::SelectionData& data, guint /*info*/, guint time) {
  if (selection_is_uri_list(data)) {
    g_signal_stop_emission_by_name(text_view_.gobj(), "drag-data-received");
    if (context && gdk_drag_context_get_protocol(context->gobj()) !=
                       GDK_DRAG_PROTO_NONE) {
      context->drag_finish(true, false, time);
    }
    open_dropped_uris(data.get_uris());
    return;
  }
  const int len = data.get_length();
  if (len > 0 && static_cast<std::size_t>(len) > max_paste_bytes()) {
    g_signal_stop_emission_by_name(text_view_.gobj(), "drag-data-received");
    report_error("Paste is too large.",
                 "A single paste is limited to " +
                     format_bytes(max_paste_bytes()) + ".");
  }
}

void MainWindow::on_window_drag_data_received(
    const Glib::RefPtr<Gdk::DragContext>& context, int /*x*/, int /*y*/,
    const Gtk::SelectionData& data, guint /*info*/, guint time) {
  if (!selection_is_uri_list(data)) {
    return;
  }
  g_signal_stop_emission_by_name(gobj(), "drag-data-received");
  if (context && gdk_drag_context_get_protocol(context->gobj()) !=
                     GDK_DRAG_PROTO_NONE) {
    context->drag_finish(true, false, time);
  }
  std::vector<Glib::ustring> uris = data.get_uris();
  if (uris.empty()) {
    const char* raw = reinterpret_cast<const char*>(data.get_data());
    if (raw != nullptr && data.get_length() > 0) {
      std::string text(raw, static_cast<std::size_t>(data.get_length()));
      std::size_t pos = 0;
      while (pos < text.size()) {
        std::size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) {
          end = text.size();
        }
        if (end > pos) {
          uris.emplace_back(text.substr(pos, end - pos));
        }
        pos = end + 1;
      }
    }
  }
  open_dropped_uris(uris);
}

void MainWindow::on_paste() {
  g_signal_emit_by_name(text_view_.gobj(), "paste-clipboard");
  update_undo_redo_sensitivity();
}

void MainWindow::on_select_all() {
  auto buf = text_view_.get_buffer();
  buf->select_range(buf->begin(), buf->end());
}

void MainWindow::remember_recent(const std::string& path) {
  app_.remember_recent(path);
}

void MainWindow::rebuild_recents_menu() {
  if (!recents_menu_) {
    return;
  }
  for (auto* child : recents_menu_->get_children()) {
    recents_menu_->remove(*child);
  }
  const auto& recents = app_.recents();
  if (recents.empty()) {
    auto* empty = Gtk::manage(new Gtk::MenuItem("(No recent files)"));
    empty->set_sensitive(false);
    recents_menu_->append(*empty);
  } else {
    for (const auto& path : recents) {
      auto* item = Gtk::manage(new Gtk::MenuItem(path));
      item->signal_activate().connect(
          [this, path]() { on_open_recent(path); });
      recents_menu_->append(*item);
    }
  }
  recents_menu_->show_all();
}

void MainWindow::set_encoding(const std::string& encoding) {
  if (encoding_ == encoding) {
    update_status();
    return;
  }
  encoding_ = encoding;
  if (!seeding_) {
    encoding_dirty_ = (encoding_ != saved_encoding_);
    refresh_dirty_from_buffer();
  }
  update_status();
}

void MainWindow::set_open_preference(bool utf8) {
  prefer_utf8_ = utf8;
  open_charset_ = utf8 ? "UTF-8" : "ISO-8859-1";
  app_.set_open_charset(open_charset_, prefer_utf8_);
}

void MainWindow::sync_encoding_radios() {
  const bool was = seeding_;
  seeding_ = true;
  if (encoding_ == "ISO-8859-1") {
    if (enc_latin1_item_) {
      enc_latin1_item_->set_active(true);
    }
  } else if (enc_utf8_item_) {
    enc_utf8_item_->set_active(true);
  }
  seeding_ = was;
}

void MainWindow::on_encoding_utf8() {
  if (enc_utf8_item_ && enc_utf8_item_->get_active()) {
    if (seeding_) {
      return;
    }
    set_encoding("UTF-8");
  }
}

void MainWindow::on_encoding_latin1() {
  if (enc_latin1_item_ && enc_latin1_item_->get_active()) {
    if (seeding_) {
      return;
    }
    set_encoding("ISO-8859-1");
  }
}

void MainWindow::on_open_pref_utf8() {
  if (open_utf8_item_ && open_utf8_item_->get_active() && !seeding_) {
    set_open_preference(true);
  }
}

void MainWindow::on_open_pref_latin1() {
  if (open_latin1_item_ && open_latin1_item_->get_active() && !seeding_) {
    set_open_preference(false);
  }
}

void MainWindow::remember_source_lines(const Glib::ustring& text,
                                       const std::vector<char>& kinds) {
  source_lines_.clear();
  const auto parts = split_lf_lines(std::string(text.data(), text.bytes()));
  source_lines_.reserve(parts.size() + 1);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    SourceLine line;
    line.text = parts[i].text;
    if (i < kinds.size()) {
      line.kind = kinds[i];
    } else {
      line.kind = parts[i].nl ? style_kind(newline_style_) : 0;
    }
    source_lines_.push_back(std::move(line));
  }
  // GtkTextBuffer counts a trailing newline as an extra empty line. Keep a
  // kind-0 entry for it so later inserts land on the same index.
  if (!source_lines_.empty() && source_lines_.back().kind != 0) {
    SourceLine phantom;
    phantom.kind = 0;
    source_lines_.push_back(std::move(phantom));
  }
}

void MainWindow::clear_ending_history() {
  ending_undo_.clear();
  ending_redo_.clear();
  pending_kinds_.clear();
  have_pending_kinds_ = false;
  ending_snapshotted_ = false;
}

std::vector<char> MainWindow::ending_kinds() const {
  std::vector<char> kinds;
  kinds.reserve(source_lines_.size());
  for (const auto& line : source_lines_) {
    kinds.push_back(line.kind);
  }
  return kinds;
}

void MainWindow::apply_ending_kinds(const std::vector<char>& kinds) {
  source_lines_.resize(kinds.size());
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    source_lines_[i].kind = kinds[i];
  }
}

void MainWindow::snapshot_endings() {
  if (ending_restore_ || seeding_ || source_lines_.empty()) {
    return;
  }
  if (in_user_action_ && ending_snapshotted_) {
    return;
  }
  ending_undo_.push_back(ending_kinds());
  if (ending_undo_.size() > 100) {
    ending_undo_.erase(ending_undo_.begin());
  }
  ending_redo_.clear();
  if (in_user_action_) {
    ending_snapshotted_ = true;
  }
}

void MainWindow::track_inserted_endings(const Gtk::TextIter& pos,
                                       const Glib::ustring& text) {
  if (seeding_ || ending_restore_ || source_lines_.empty()) {
    return;
  }
  auto buf = text_view_.get_buffer();
  if (!buf ||
      static_cast<int>(source_lines_.size()) != buf->get_line_count()) {
    source_lines_.clear();
    clear_ending_history();
    return;
  }
  snapshot_endings();
  const int newlines =
      static_cast<int>(count_newlines(text.data(), text.bytes()));
  if (newlines <= 0) {
    return;
  }
  const int line = pos.get_line();
  if (line < 0 || line > static_cast<int>(source_lines_.size())) {
    source_lines_.clear();
    clear_ending_history();
    return;
  }
  // The inserted breaks are new, so they take the document's dominant
  // style. The line that was split keeps the ending that was already there,
  // which shifts right with the text after the insertion point.
  SourceLine added;
  added.kind = style_kind(newline_style_);
  source_lines_.insert(source_lines_.begin() + line,
                       static_cast<std::size_t>(newlines), added);
}

void MainWindow::track_erased_endings(const Gtk::TextIter& start,
                                     const Gtk::TextIter& end) {
  if (seeding_ || ending_restore_ || source_lines_.empty()) {
    return;
  }
  auto buf = text_view_.get_buffer();
  if (!buf ||
      static_cast<int>(source_lines_.size()) != buf->get_line_count()) {
    source_lines_.clear();
    clear_ending_history();
    return;
  }
  snapshot_endings();
  const int start_line = start.get_line();
  const int end_line = end.get_line();
  if (start_line == end_line) {
    return;
  }
  if (start_line < 0 || end_line < start_line ||
      end_line > static_cast<int>(source_lines_.size())) {
    source_lines_.clear();
    clear_ending_history();
    return;
  }
  // Each deleted newline drops that line's ending. The line the deletion
  // ends on keeps its own ending and receives the surviving text.
  source_lines_.erase(source_lines_.begin() + start_line,
                      source_lines_.begin() + end_line);
}

void MainWindow::remember_file_identity(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    have_file_id_ = false;
    return;
  }
  have_file_id_ = true;
  file_dev_ = static_cast<std::uint64_t>(st.st_dev);
  file_ino_ = static_cast<std::uint64_t>(st.st_ino);
  file_mtime_sec_ = static_cast<std::int64_t>(st.st_mtim.tv_sec);
  file_mtime_nsec_ = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
}

MainWindow::DiskChangeChoice MainWindow::confirm_file_changed(
    const std::string& /*path*/) {
  if (const char* choice = test_replace()) {
    if (std::strcmp(choice, "reload") == 0) {
      return DiskChangeChoice::Reload;
    }
    if (std::strcmp(choice, "allow") == 0) {
      return DiskChangeChoice::Replace;
    }
    return DiskChangeChoice::Cancel;
  }
  if (test_mode()) {
    return DiskChangeChoice::Replace;
  }
  Gtk::MessageDialog dlg(*this, "This file changed on disk.", false,
                         Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text(
      "\"" + current_basename() +
      "\" was modified after it was opened. Replace it with the text in "
      "this window, or reload the copy on disk?");
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Reload", Gtk::RESPONSE_APPLY);
  dlg.add_button("_Replace", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_CANCEL);
  const int resp = run_modal(app_, dlg);
  if (resp == Gtk::RESPONSE_ACCEPT) {
    return DiskChangeChoice::Replace;
  }
  if (resp == Gtk::RESPONSE_APPLY) {
    return DiskChangeChoice::Reload;
  }
  return DiskChangeChoice::Cancel;
}

bool MainWindow::buffer_has_long_line() {
  if (long_line_present_) {
    return true;
  }
  auto buf = buffer();
  if (!buf) {
    return false;
  }
  const int lines = buf->get_line_count();
  for (int i = 0; i < lines; ++i) {
    if (buf->get_iter_at_line(i).get_chars_in_line() >= kLongLineChars) {
      long_line_present_ = true;
      return true;
    }
  }
  return false;
}

void MainWindow::note_line_length(int chars_in_line) {
  if (chars_in_line >= kLongLineChars) {
    long_line_present_ = true;
  }
}

void MainWindow::sync_long_line_window() {
  if (syncing_long_line_ || !long_hidden_tag_) {
    return;
  }
  auto buf = buffer();
  if (!buf) {
    return;
  }

  auto preserve_modified = [&](const bool modified) {
    if (buf->get_modified() != modified) {
      buf->set_modified(modified);
    }
  };
  auto clear_hide = [&]() {
    syncing_long_line_ = true;
    const bool modified = buf->get_modified();
    buf->remove_tag(long_hidden_tag_, buf->begin(), buf->end());
    preserve_modified(modified);
    syncing_long_line_ = false;
    long_window_line_ = -1;
    long_window_begin_ = 0;
    long_window_end_ = 0;
  };

  if (text_view_.get_wrap_mode() != Gtk::WRAP_NONE) {
    long_line_present_ = false;
    if (long_window_line_ >= 0) {
      clear_hide();
    }
    return;
  }

  auto iter = buf->get_iter_at_mark(buf->get_insert());
  const int line = iter.get_line();
  Gtk::TextIter line_start = buf->get_iter_at_line(line);
  int chars = line_start.get_chars_in_line();
  const bool has_break = line + 1 < buf->get_line_count();
  int content = chars;
  if (has_break && content > 0) {
    --content;
  }
  if (content >= kLongLineChars) {
    long_line_present_ = true;
  }

  if (content <= kLongLineWindow) {
    if (long_window_line_ == line) {
      clear_hide();
      if (content < kLongLineChars) {
        long_line_present_ = false;
      }
    }
    return;
  }

  long_line_present_ = true;
  int line_off = iter.get_line_offset();
  if (line_off > content) {
    line_off = content;
  }
  int vis_begin = line_off - kLongLineWindow / 2;
  if (vis_begin < 0) {
    vis_begin = 0;
  }
  int vis_end = vis_begin + kLongLineWindow;
  if (vis_end > content) {
    vis_end = content;
    vis_begin = std::max(0, vis_end - kLongLineWindow);
  }

  const int line_start_off = line_start.get_offset();
  syncing_long_line_ = true;
  const bool modified = buf->get_modified();
  if (long_window_line_ == line && long_window_end_ > long_window_begin_) {
    if (vis_begin > long_window_begin_) {
      buf->apply_tag(long_hidden_tag_,
                     buf->get_iter_at_offset(line_start_off + long_window_begin_),
                     buf->get_iter_at_offset(line_start_off + vis_begin));
    } else if (vis_begin < long_window_begin_) {
      buf->remove_tag(long_hidden_tag_,
                      buf->get_iter_at_offset(line_start_off + vis_begin),
                      buf->get_iter_at_offset(line_start_off + long_window_begin_));
    }
    if (vis_end < long_window_end_) {
      buf->apply_tag(long_hidden_tag_,
                     buf->get_iter_at_offset(line_start_off + vis_end),
                     buf->get_iter_at_offset(line_start_off + long_window_end_));
    } else if (vis_end > long_window_end_) {
      buf->remove_tag(long_hidden_tag_,
                      buf->get_iter_at_offset(line_start_off + long_window_end_),
                      buf->get_iter_at_offset(line_start_off + vis_end));
    }
  } else {
    buf->remove_tag(long_hidden_tag_, buf->begin(), buf->end());
    if (vis_begin > 0) {
      buf->apply_tag(long_hidden_tag_, line_start,
                     buf->get_iter_at_offset(line_start_off + vis_begin));
    }
    if (vis_end < content) {
      buf->apply_tag(long_hidden_tag_,
                     buf->get_iter_at_offset(line_start_off + vis_end),
                     buf->get_iter_at_offset(line_start_off + content));
    }
  }
  long_window_line_ = line;
  long_window_begin_ = vis_begin;
  long_window_end_ = vis_end;
  preserve_modified(modified);
  syncing_long_line_ = false;
}

void MainWindow::maybe_restore_wrap() {
  if (seeding_ || !app_.wrap_text()) {
    return;
  }
  if (text_view_.get_wrap_mode() != Gtk::WRAP_NONE) {
    return;
  }
  if (buffer_has_long_line()) {
    return;
  }
  suppress_wrap_pref_ = true;
  text_view_.set_wrap_mode(Gtk::WRAP_WORD_CHAR);
  if (wrap_item_ && !wrap_item_->get_active()) {
    wrap_item_->set_active(true);
  }
  suppress_wrap_pref_ = false;
}

Gtk::TextSearchFlags MainWindow::search_flags(const FindOptions& opts) const {
  Gtk::TextSearchFlags flags = Gtk::TEXT_SEARCH_TEXT_ONLY;
  if (!opts.case_sensitive) {
    flags |= Gtk::TEXT_SEARCH_CASE_INSENSITIVE;
  }
  return flags;
}

bool MainWindow::is_entire_word(const Gtk::TextIter& start,
                                const Gtk::TextIter& end) const {
  if (start.editable() && !start.starts_word() && !start.inside_word()) {
    // starts at non-word is ok if previous isn't word char — use starts_word
  }
  auto s = start;
  auto e = end;
  // Word boundary: start is start-of-word or start-of-buffer / non-word before
  bool left_ok = s.starts_word() || s.is_start();
  if (!left_ok) {
    auto prev = s;
    if (prev.backward_char()) {
      gunichar c = prev.get_char();
      left_ok = !g_unichar_isalnum(c) && c != '_' && c != '-';
    } else {
      left_ok = true;
    }
  }
  bool right_ok = e.ends_word() || e.is_end();
  if (!right_ok) {
    gunichar c = e.get_char();
    right_ok = (c == 0) || (!g_unichar_isalnum(c) && c != '_' && c != '-');
  }
  return left_ok && right_ok;
}

void MainWindow::ensure_find_marks() {
  auto buf = text_view_.get_buffer();
  if (!sel_only_start_mark_) {
    sel_only_start_mark_ =
        buf->create_mark("lunduke-sel-only-start", buf->begin(), true);
    sel_only_end_mark_ =
        buf->create_mark("lunduke-sel-only-end", buf->begin(), false);
  }
  if (!extend_anchor_mark_) {
    extend_anchor_mark_ =
        buf->create_mark("lunduke-extend-anchor", buf->begin(), true);
  }
  if (!last_match_start_) {
    last_match_start_ =
        buf->create_mark("lunduke-last-match-start", buf->begin(), true);
    last_match_end_ =
        buf->create_mark("lunduke-last-match-end", buf->begin(), false);
  }
}

void MainWindow::pin_selection_only_range() {
  auto buf = text_view_.get_buffer();
  Gtk::TextIter a, b;
  if (!buf->get_selection_bounds(a, b) || a == b) {
    return;
  }
  ensure_find_marks();
  buf->move_mark(sel_only_start_mark_, a);
  buf->move_mark(sel_only_end_mark_, b);
  sel_only_range_valid_ = true;
}

void MainWindow::clear_selection_only_range() {
  sel_only_range_valid_ = false;
}

void MainWindow::clear_extend_anchor() {
  extend_anchor_valid_ = false;
}

bool MainWindow::selection_matches_needle(const FindOptions& opts,
                                          const Gtk::TextIter& a,
                                          const Gtk::TextIter& b) const {
  Glib::ustring selected = a.get_text(b);
  Glib::ustring needle = opts.search_for;
  if (!opts.case_sensitive) {
    selected = selected.casefold();
    needle = needle.casefold();
  }
  return selected == needle;
}

bool MainWindow::get_search_bounds(const FindOptions& opts, Gtk::TextIter& begin,
                                   Gtk::TextIter& end) {
  auto buf = text_view_.get_buffer();
  if (opts.search_selection_only) {
    // Prefer the range pinned when Find opened / Selection Only engaged so a
    // successful match (which reselection) does not shrink the search scope.
    if (sel_only_range_valid_ && sel_only_start_mark_ && sel_only_end_mark_) {
      begin = buf->get_iter_at_mark(sel_only_start_mark_);
      end = buf->get_iter_at_mark(sel_only_end_mark_);
      if (begin < end) {
        return true;
      }
    }
    Gtk::TextIter sel_a, sel_b;
    if (buf->get_selection_bounds(sel_a, sel_b) && sel_a != sel_b) {
      begin = sel_a;
      end = sel_b;
      ensure_find_marks();
      buf->move_mark(sel_only_start_mark_, sel_a);
      buf->move_mark(sel_only_end_mark_, sel_b);
      sel_only_range_valid_ = true;
      return true;
    }
    // No selection: do not fall through to the whole buffer.
    begin = buf->begin();
    end = begin;
    return false;
  }
  begin = buf->begin();
  end = buf->end();
  return true;
}

bool MainWindow::find_match(const FindOptions& opts, bool from_next) {
  struct Reentry {
    Application& app;
    explicit Reentry(Application& a) : app(a) { app.push_reentry(); }
    ~Reentry() { app.pop_reentry(); }
  } reentry(app_);
  if (opts.search_for.empty()) {
    return false;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);

  if (!opts.search_selection_only) {
    clear_selection_only_range();
  }
  if (!opts.extend_selection) {
    clear_extend_anchor();
  }

  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(opts, range_begin, range_end)) {
    return false;
  }

  Gtk::TextIter start;
  Gtk::TextIter sel_a, sel_b;
  const bool have_sel =
      buf->get_selection_bounds(sel_a, sel_b) && sel_a != sel_b;
  // Extend continues past an existing selection/anchor so the span can grow.
  // With no selection yet, Start at Top still applies (FR-B01 / first hit).
  // When Extend is off, Start at Top is unchanged (FR-D03).
  const bool extending_existing =
      opts.extend_selection && (have_sel || extend_anchor_valid_);
  const bool honor_start_at_top =
      opts.start_at_top && !from_next && !extending_existing;

  if (honor_start_at_top) {
    // Searching backward from the start of the range finds nothing.
    // "Start at top" in that direction begins at the end of the range.
    start = opts.search_backwards ? range_end : range_begin;
  } else {
    start = buf->get_iter_at_mark(buf->get_insert());

    if (opts.extend_selection && have_sel) {
      // Grow forward from selection end (or backward from selection start).
      start = opts.search_backwards ? sel_a : sel_b;
      if (!extend_anchor_valid_) {
        ensure_find_marks();
        buf->move_mark(extend_anchor_mark_,
                       opts.search_backwards ? sel_b : sel_a);
        extend_anchor_valid_ = true;
      }
    } else {
      if (opts.search_selection_only) {
        if (start < range_begin || start > range_end) {
          start = opts.search_backwards ? range_end : range_begin;
        }
      }
      if (from_next || (!opts.start_at_top) || opts.extend_selection) {
        // Move past the current selection only when it is exactly the needle.
        // Stepping an extra character when it is not makes forward_search
        // miss a match that starts at the cursor (the second "x" in "xx",
        // and Wrap Around then jumps to an earlier hit).
        if (have_sel && selection_matches_needle(opts, sel_a, sel_b)) {
          start = opts.search_backwards ? sel_a : sel_b;
        }
      }
    }

    if (opts.search_selection_only) {
      if (start < range_begin) {
        start = range_begin;
      }
      if (start > range_end) {
        start = range_end;
      }
    }
  }

  auto try_search = [&](const Gtk::TextIter& from, bool wrap_pass) -> bool {
    Gtk::TextIter match_start, match_end;
    Gtk::TextIter cursor = from;
    const int guard = buf->get_char_count() + 2;
    const int extend =
        std::max(0, static_cast<int>(opts.search_for.length()) - 1);
    auto yield_slice = []() {
      // One turn so a timer can run, then a few already-queued events.
      g_main_context_iteration(nullptr, FALSE);
      int spins = 0;
      while (g_main_context_pending(nullptr) && spins < 8) {
        g_main_context_iteration(nullptr, FALSE);
        ++spins;
      }
    };
    for (int i = 0; i < guard; ++i) {
      bool found = false;
      bool limited = false;
      Gtk::TextIter slice_bound = cursor;
      Gtk::TextIter search_limit;
      if (opts.search_backwards) {
        if (slice_bound.backward_chars(kFindSliceChars) &&
            slice_bound > range_begin) {
          limited = true;
        } else if (slice_bound < range_begin) {
          slice_bound = range_begin;
        }
        if (slice_bound < range_begin) {
          slice_bound = range_begin;
        }
        if (!limited) {
          slice_bound = range_begin;
        }
        search_limit = slice_bound;
        if (limited && extend > 0) {
          Gtk::TextIter ext = slice_bound;
          ext.backward_chars(extend);
          if (ext < range_begin) {
            ext = range_begin;
          }
          search_limit = ext;
        }
        found = cursor.backward_search(opts.search_for, flags, match_start,
                                       match_end, search_limit);
      } else {
        if (slice_bound.forward_chars(kFindSliceChars) &&
            slice_bound < range_end) {
          limited = true;
        }
        if (slice_bound > range_end) {
          slice_bound = range_end;
          limited = false;
        }
        if (!limited) {
          slice_bound = range_end;
        }
        search_limit = slice_bound;
        if (limited && extend > 0) {
          Gtk::TextIter ext = slice_bound;
          int left = extend;
          while (left > 0 && ext < range_end && ext.forward_char()) {
            --left;
          }
          search_limit = ext;
        }
        found = cursor.forward_search(opts.search_for, flags, match_start,
                                      match_end, search_limit);
      }
      if (!found) {
        if (!limited) {
          return false;
        }
        cursor = slice_bound;
        yield_slice();
        continue;
      }
      if (!opts.search_selection_only ||
          (match_start >= range_begin && match_end <= range_end)) {
        if (!opts.entire_word || is_entire_word(match_start, match_end)) {
          if (opts.extend_selection) {
            ensure_find_marks();
            if (!extend_anchor_valid_) {
              // Anchor at the far end opposite the search direction so growth
              // keeps the original hit while adding new matches.
              Gtk::TextIter cur_a, cur_b;
              if (buf->get_selection_bounds(cur_a, cur_b) && cur_a != cur_b) {
                buf->move_mark(extend_anchor_mark_,
                               opts.search_backwards ? cur_b : cur_a);
              } else {
                buf->move_mark(extend_anchor_mark_, match_start);
              }
              extend_anchor_valid_ = true;
            }
            Gtk::TextIter anchor = buf->get_iter_at_mark(extend_anchor_mark_);
            Gtk::TextIter ext_a =
                anchor < match_start ? anchor : match_start;
            Gtk::TextIter ext_b = anchor > match_end ? anchor : match_end;
            Gtk::TextIter cur_a, cur_b;
            if (buf->get_selection_bounds(cur_a, cur_b) && cur_a != cur_b) {
              if (cur_a < ext_a) {
                ext_a = cur_a;
              }
              if (cur_b > ext_b) {
                ext_b = cur_b;
              }
            }
            buf->select_range(ext_a, ext_b);
          } else {
            buf->select_range(match_start, match_end);
          }
          ensure_find_marks();
          buf->move_mark(last_match_start_, match_start);
          buf->move_mark(last_match_end_, match_end);
          last_match_valid_ = true;
          text_view_.scroll_to(match_start);
          update_status();
          return true;
        }
      }
      cursor = opts.search_backwards ? match_start : match_end;
      if (opts.search_backwards) {
        if (!cursor.backward_char()) {
          break;
        }
      }
      if (opts.entire_word &&
          (i + 1) % std::max(1, find_chunk_size()) == 0) {
        int spins = 0;
        while (g_main_context_pending(nullptr) && spins < 8) {
          g_main_context_iteration(nullptr, false);
          ++spins;
        }
      }
      (void)wrap_pass;
    }
    return false;
  };

  if (try_search(start, false)) {
    return true;
  }
  if (opts.wrap_around) {
    Gtk::TextIter wrap_from = opts.search_backwards ? range_end : range_begin;
    return try_search(wrap_from, true);
  }
  return false;
}

int MainWindow::count_matches(const FindOptions& opts) {
  if (opts.search_for.empty()) {
    return 0;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(opts, range_begin, range_end)) {
    return 0;
  }

  int count = 0;
  Gtk::TextIter cursor = range_begin;
  Gtk::TextIter match_start, match_end;
  while (cursor.forward_search(opts.search_for, flags, match_start, match_end,
                               range_end)) {
    if (!opts.entire_word || is_entire_word(match_start, match_end)) {
      ++count;
    }
    cursor = match_end;
    if (match_start == match_end) {
      if (!cursor.forward_char()) {
        break;
      }
    }
  }
  return count;
}

void MainWindow::clear_find_highlights() {
  auto buf = buffer();
  if (find_tag_ && buf) {
    buf->remove_tag(find_tag_, buf->begin(), buf->end());
  }
}

void MainWindow::set_find_count(int n, bool capped) {
  if (n < 0) {
    status_find_.set_text("");
    if (status_find_frame_.get_mapped() || status_find_frame_.get_visible()) {
      status_find_frame_.hide();
    }
    return;
  }
  std::string text = std::to_string(n);
  if (capped) {
    text += "+";
  }
  text += (n == 1 && !capped) ? " match" : " matches";
  status_find_.set_text(text);
  status_find_frame_.show();
}

void MainWindow::end_find_user_action() {
  if (!find_scan_.user_action_open) {
    return;
  }
  find_scan_.user_action_open = false;
  if (auto buf = buffer()) {
    buf->end_user_action();
  }
}

void MainWindow::cancel_find_scan() {
  const bool rollback = find_scan_.user_action_open &&
                        find_scan_.kind == FindScan::Kind::ReplaceAll;
  find_scan_.cancel = true;
  find_idle_.disconnect();
  end_find_user_action();
  if (rollback) {
    if (auto buf = buffer()) {
      if (buf->can_undo()) {
        buf->undo();
      }
    }
    refresh_dirty_from_buffer();
  }
  find_scan_.active = false;
  find_scan_.dlg = nullptr;
  find_scan_.finishing = false;
  update_undo_redo_sensitivity();
}

void MainWindow::on_find_dialog_hidden() {
  cancel_find_scan();
  clear_find_highlights();
  find_highlights_on_ = false;
  set_find_count(-1, false);
  // The selection-only range and the extend anchor belong to the dialog
  // that just closed. Find Next must not keep searching that hidden range.
  // Extend is a dialog mode. It does not stay latched after the dialog
  // is gone. Search Selection Only stays so F3 can still say when nothing
  // is selected; a selection that is only the match just found is not
  // reused as the range.
  clear_selection_only_range();
  clear_extend_anchor();
  find_opts_.extend_selection = false;
}

bool MainWindow::confirm_huge_undo(std::size_t bytes) {
  if (const char* choice = test_huge_undo_choice()) {
    return std::strcmp(choice, "allow") == 0;
  }
  if (test_mode()) {
    return true;
  }
  Gtk::MessageDialog dlg(
      find_scan_.dlg ? static_cast<Gtk::Window&>(*find_scan_.dlg)
                     : static_cast<Gtk::Window&>(*this),
      "Replace All would store a very large undo record.", false,
      Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text(
      "Continuing keeps about " + format_bytes(bytes) +
      " so the replacement can be undone. Continue?");
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Replace", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_CANCEL);
  return run_modal(app_, dlg) == Gtk::RESPONSE_ACCEPT;
}

void MainWindow::finish_find_scan(bool show_result) {
  if (find_scan_.finishing) {
    return;
  }
  find_scan_.finishing = true;
  find_idle_.disconnect();
  end_find_user_action();

  auto buf = buffer();
  const bool cancelled = find_scan_.cancel;
  const int count = find_scan_.count;
  const bool capped = find_scan_.capped;
  const auto kind = find_scan_.kind;
  FindReplaceDialog* dlg = find_scan_.dlg;

  if (kind == FindScan::Kind::FindAll && buf && find_scan_.select_start >= 0) {
    auto a = buf->get_iter_at_offset(find_scan_.select_start);
    auto b = buf->get_iter_at_offset(find_scan_.select_end);
    buf->select_range(a, b);
    text_view_.scroll_to(a);
    update_cursor_status();
  }
  if (kind == FindScan::Kind::FindAll) {
    set_find_count(count, capped);
  }

  find_scan_.active = false;
  find_scan_.dlg = nullptr;

  if (show_result && !cancelled && dlg != nullptr && !test_mode()) {
    if (kind == FindScan::Kind::FindAll) {
      Glib::ustring message =
          "Found " + std::to_string(count) + (count == 1 ? " match." : " matches.");
      if (capped) {
        message = "Highlighted the first " + std::to_string(count) +
                  " matches. The document has more.";
      }
      Gtk::MessageDialog info(*dlg, message, false, Gtk::MESSAGE_INFO,
                              Gtk::BUTTONS_OK, true);
      run_modal(app_, info);
    } else if (kind == FindScan::Kind::ReplaceAll) {
      Gtk::MessageDialog info(
          *dlg,
          "Replaced " + std::to_string(count) +
              (count == 1 ? " occurrence." : " occurrences."),
          false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
      run_modal(app_, info);
    }
  }
  update_undo_redo_sensitivity();
  update_status();
  if (kind == FindScan::Kind::FindAll) {
    // update_status does not touch the match label; put it back if a
    // cursor update ran above. set_find_count is idempotent.
    set_find_count(count, capped);
  }
}

MainWindow::SearchStep MainWindow::step_search(bool backward, int& cursor_off,
                                               int& match_start,
                                               int& match_end) {
  auto buf = buffer();
  if (!buf || find_scan_.opts.search_for.empty()) {
    return SearchStep::Miss;
  }
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
    return SearchStep::Miss;
  }
  const auto flags = search_flags(find_scan_.opts);
  const int limit = std::max(1, find_chunk_size());
  Gtk::TextIter cursor = buf->get_iter_at_offset(cursor_off);
  if (cursor < range_begin) {
    cursor = range_begin;
  }
  if (cursor > range_end) {
    cursor = range_end;
  }
  int scanned = 0;
  int previous = -1;
  while (scanned < limit) {
    if (find_scan_.cancel) {
      find_scan_.slice_steps = scanned;
      return SearchStep::Miss;
    }
    ++scanned;
    Gtk::TextIter ms, me;
    bool found = false;
    if (backward) {
      found = cursor.backward_search(find_scan_.opts.search_for, flags, ms, me,
                                     range_begin);
    } else {
      found = cursor.forward_search(find_scan_.opts.search_for, flags, ms, me,
                                    range_end);
    }
    if (!found) {
      find_scan_.slice_steps = scanned;
      cursor_off = cursor.get_offset();
      return SearchStep::Miss;
    }
    const bool in_range = !find_scan_.opts.search_selection_only ||
                          (ms >= range_begin && me <= range_end);
    const bool word_ok =
        !find_scan_.opts.entire_word || is_entire_word(ms, me);
    if (in_range && word_ok) {
      match_start = ms.get_offset();
      match_end = me.get_offset();
      find_scan_.slice_steps = scanned;
      cursor_off = cursor.get_offset();
      return SearchStep::Hit;
    }
    if (backward) {
      cursor = ms;
      if (!cursor.backward_char()) {
        find_scan_.slice_steps = scanned;
        cursor_off = cursor.get_offset();
        return SearchStep::Miss;
      }
    } else {
      cursor = me;
      if (ms == me && !cursor.forward_char()) {
        find_scan_.slice_steps = scanned;
        cursor_off = cursor.get_offset();
        return SearchStep::Miss;
      }
    }
    const int at = cursor.get_offset();
    if (at == previous) {
      find_scan_.slice_steps = scanned;
      cursor_off = at;
      return SearchStep::Miss;
    }
    previous = at;
    cursor_off = at;
  }
  find_scan_.slice_steps = scanned;
  return SearchStep::Yield;
}

bool MainWindow::pump_find_highlight() {
  if (!find_scan_.active || find_scan_.cancel) {
    finish_find_scan(false);
    return false;
  }
  auto buf = buffer();
  if (!buf) {
    finish_find_scan(false);
    return false;
  }
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
    finish_find_scan(true);
    return false;
  }
  const bool backward = find_scan_.opts.search_backwards;
  if (!find_scan_.started) {
    find_scan_.started = true;
    find_scan_.cursor_off =
        backward ? range_end.get_offset() : range_begin.get_offset();
    clear_find_highlights();
    find_highlights_on_ = false;
    find_scan_.select_start = -1;
  }

  const int cap = max_find_hits();
  const int chunk = find_chunk_size();
  int cursor_off = find_scan_.cursor_off;
  for (int n = 0; n < chunk; ++n) {
    if (find_scan_.count >= cap) {
      int extra_s = 0;
      int extra_e = 0;
      int peek = cursor_off;
      const SearchStep extra =
          step_search(backward, peek, extra_s, extra_e);
      if (extra == SearchStep::Yield) {
        find_scan_.cursor_off = peek;
        return true;
      }
      if (extra == SearchStep::Hit) {
        find_scan_.capped = true;
      }
      find_scan_.cursor_off = cursor_off;
      finish_find_scan(true);
      return false;
    }
    int ms = 0;
    int me = 0;
    const SearchStep step = step_search(backward, cursor_off, ms, me);
    if (step == SearchStep::Yield) {
      find_scan_.cursor_off = cursor_off;
      return true;
    }
    if (step != SearchStep::Hit) {
      find_scan_.cursor_off = cursor_off;
      finish_find_scan(true);
      return false;
    }
    if (find_tag_) {
      buf->apply_tag(find_tag_, buf->get_iter_at_offset(ms),
                     buf->get_iter_at_offset(me));
      find_highlights_on_ = true;
    }
    if (find_scan_.select_start < 0) {
      // First hit in search order: the last match when searching backward.
      find_scan_.select_start = ms;
      find_scan_.select_end = me;
    }
    ++find_scan_.count;
    if (backward) {
      // The next backward search must start at this match. Starting one
      // character earlier hides a match that ends at match_start, so
      // adjacent hits such as the letters in "aaaa" are skipped.
      if (ms == me) {
        Gtk::TextIter next = buf->get_iter_at_offset(ms);
        if (!next.backward_char()) {
          find_scan_.cursor_off = next.get_offset();
          finish_find_scan(true);
          return false;
        }
        cursor_off = next.get_offset();
      } else {
        cursor_off = ms;
      }
    } else {
      Gtk::TextIter next = buf->get_iter_at_offset(me);
      if (ms == me && !next.forward_char()) {
        find_scan_.cursor_off = next.get_offset();
        finish_find_scan(true);
        return false;
      }
      cursor_off = next.get_offset();
    }
  }
  find_scan_.cursor_off = cursor_off;
  return true;
}

bool MainWindow::pump_replace() {
  if (!find_scan_.active || find_scan_.cancel) {
    finish_find_scan(false);
    return false;
  }
  auto buf = buffer();
  if (!buf || find_scan_.opts.search_for.empty()) {
    finish_find_scan(true);
    return false;
  }

  const std::size_t per = find_scan_.opts.search_for.bytes() +
                          find_scan_.opts.replace_with.bytes();
  const std::size_t limit = huge_undo_limit();

  if (find_scan_.counting) {
    Gtk::TextIter range_begin, range_end;
    if (!find_scan_.started) {
      if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
        finish_find_scan(true);
        return false;
      }
      find_scan_.started = true;
      find_scan_.cursor_off = range_begin.get_offset();
      find_scan_.count = 0;
      find_scan_.undo_bytes = 0;
    }
    const int chunk = find_chunk_size();
    int cursor_off = find_scan_.cursor_off;
    for (int n = 0; n < chunk; ++n) {
      int ms = 0;
      int me = 0;
      const SearchStep step = step_search(false, cursor_off, ms, me);
      if (step == SearchStep::Yield) {
        find_scan_.cursor_off = cursor_off;
        return true;
      }
      if (step != SearchStep::Hit) {
        const std::size_t estimated = find_scan_.undo_bytes;
        find_scan_.counting = false;
        find_scan_.started = false;
        find_scan_.cursor_off = 0;
        find_scan_.count = 0;
        if (estimated >= limit && limit > 0) {
          find_idle_.disconnect();
          if (!confirm_huge_undo(estimated) || find_scan_.cancel) {
            finish_find_scan(false);
            return false;
          }
          find_idle_ = Glib::signal_idle().connect(
              sigc::mem_fun(*this, &MainWindow::on_find_idle));
          return false;
        }
        return true;
      }
      find_scan_.undo_bytes += per;
      ++find_scan_.count;
      if (find_scan_.undo_bytes >= limit && limit > 0) {
        const std::size_t estimated = find_scan_.undo_bytes;
        find_scan_.counting = false;
        find_scan_.started = false;
        find_scan_.count = 0;
        find_idle_.disconnect();
        if (!confirm_huge_undo(estimated) || find_scan_.cancel) {
          finish_find_scan(false);
          return false;
        }
        find_idle_ = Glib::signal_idle().connect(
            sigc::mem_fun(*this, &MainWindow::on_find_idle));
        return false;
      }
      Gtk::TextIter next = buf->get_iter_at_offset(me);
      if (ms == me && !next.forward_char()) {
        find_scan_.counting = false;
        find_scan_.started = false;
        find_scan_.count = 0;
        return true;
      }
      cursor_off = next.get_offset();
    }
    find_scan_.cursor_off = cursor_off;
    return true;
  }

  // Collect every hit before touching the buffer. The first idle must not
  // edit, so Cancel after it leaves the text alone. The commit is one
  // erase and one insert, which is one undo step.
  if (!find_scan_.collected) {
    if (!find_scan_.started) {
      Gtk::TextIter range_begin, range_end;
      if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
        finish_find_scan(true);
        return false;
      }
      find_scan_.started = true;
      find_scan_.cursor_off = range_begin.get_offset();
      find_scan_.replace_start = range_begin.get_offset();
      find_scan_.replace_end = range_end.get_offset();
      find_scan_.count = 0;
      find_scan_.hits.clear();
    }
    const int chunk = find_chunk_size();
    int cursor_off = find_scan_.cursor_off;
    for (int n = 0; n < chunk; ++n) {
      int ms = 0;
      int me = 0;
      const SearchStep step = step_search(false, cursor_off, ms, me);
      if (step == SearchStep::Yield) {
        find_scan_.cursor_off = cursor_off;
        return true;
      }
      if (step != SearchStep::Hit) {
        find_scan_.collected = true;
        find_scan_.cursor_off = cursor_off;
        return true;
      }
      find_scan_.hits.emplace_back(ms, me);
      ++find_scan_.count;
      Gtk::TextIter next = buf->get_iter_at_offset(me);
      if (ms == me && !next.forward_char()) {
        find_scan_.collected = true;
        return true;
      }
      cursor_off = next.get_offset();
    }
    find_scan_.cursor_off = cursor_off;
    return true;
  }

  const int count = static_cast<int>(find_scan_.hits.size());
  find_scan_.count = count;
  if (count > 0) {
    const Glib::ustring whole = buf->get_text();
    const Glib::ustring& repl = find_scan_.opts.replace_with;
    Glib::ustring neu;
    int cursor = find_scan_.replace_start;
    const int end = find_scan_.replace_end;
    neu.reserve(static_cast<std::size_t>(std::max(0, end - cursor)) +
                static_cast<std::size_t>(count) * repl.length());
    for (const auto& hit : find_scan_.hits) {
      if (hit.first > cursor) {
        neu.append(whole.substr(
            static_cast<Glib::ustring::size_type>(cursor),
            static_cast<Glib::ustring::size_type>(hit.first - cursor)));
      }
      neu.append(repl);
      cursor = hit.second;
    }
    if (cursor < end) {
      neu.append(whole.substr(
          static_cast<Glib::ustring::size_type>(cursor),
          static_cast<Glib::ustring::size_type>(end - cursor)));
    }
    apply_bulk_replace(find_scan_.replace_start, end, neu);
  }
  find_scan_.hits.clear();
  finish_find_scan(true);
  return false;
}

bool MainWindow::on_find_idle() {
  if (!find_scan_.active || find_scan_.finishing) {
    return false;
  }
  if (find_scan_.kind == FindScan::Kind::ReplaceAll) {
    return pump_replace();
  }
  return pump_find_highlight();
}

void MainWindow::start_find_all(const FindOptions& opts, FindReplaceDialog* dlg) {
  cancel_find_scan();
  find_scan_ = FindScan{};
  find_scan_.kind = FindScan::Kind::FindAll;
  find_scan_.active = true;
  find_scan_.opts = opts;
  find_scan_.dlg = dlg;
  if (opts.search_for.empty()) {
    finish_find_scan(true);
    return;
  }
  find_idle_ = Glib::signal_idle().connect(
      sigc::mem_fun(*this, &MainWindow::on_find_idle));
}

void MainWindow::start_replace_all(const FindOptions& opts,
                                   FindReplaceDialog* dlg) {
  cancel_find_scan();
  find_scan_ = FindScan{};
  find_scan_.kind = FindScan::Kind::ReplaceAll;
  find_scan_.active = true;
  find_scan_.opts = opts;
  find_scan_.dlg = dlg;
  if (opts.search_for.empty()) {
    finish_find_scan(true);
    return;
  }
  const std::size_t per =
      opts.search_for.bytes() + opts.replace_with.bytes();
  const std::size_t limit = huge_undo_limit();
  const std::size_t needle = std::max<std::size_t>(opts.search_for.bytes(), 1);
  const std::size_t max_matches = utf8_bytes_ / needle + 1;
  const bool maybe_huge =
      limit > 0 && per > 0 && max_matches > limit / per;
  find_scan_.counting = maybe_huge;
  find_idle_ = Glib::signal_idle().connect(
      sigc::mem_fun(*this, &MainWindow::on_find_idle));
}

void MainWindow::highlight_all_matches(const FindOptions& opts) {
  cancel_find_scan();
  find_scan_ = FindScan{};
  find_scan_.kind = FindScan::Kind::FindAll;
  find_scan_.active = true;
  find_scan_.opts = opts;
  if (opts.search_for.empty()) {
    finish_find_scan(false);
    return;
  }
  while (find_scan_.active && pump_find_highlight()) {
  }
}

MainWindow::ReplaceResult MainWindow::replace_current(const FindOptions& opts) {
  auto buf = text_view_.get_buffer();
  auto matches_needle = [&](const Gtk::TextIter& a, const Gtk::TextIter& b) {
    return a != b && selection_matches_needle(opts, a, b) &&
           (!opts.entire_word || is_entire_word(a, b));
  };
  auto replace_span = [&](int start_off, int end_off) {
    buf->begin_user_action();
    auto a = buf->get_iter_at_offset(start_off);
    auto b = buf->get_iter_at_offset(end_off);
    buf->erase(a, b);
    a = buf->get_iter_at_offset(start_off);
    buf->insert(a, opts.replace_with);
    buf->end_user_action();
    last_match_valid_ = false;
    update_undo_redo_sensitivity();
    find_match(opts, true);
  };

  Gtk::TextIter sel_a, sel_b;
  const bool have_sel =
      buf->get_selection_bounds(sel_a, sel_b) && sel_a != sel_b;
  if (have_sel && matches_needle(sel_a, sel_b)) {
    replace_span(sel_a.get_offset(), sel_b.get_offset());
    return ReplaceResult::Replaced;
  }

  // Extend Selection grows the selection past the needle. Replace the match
  // that was found, not the grown span, and do not search again (that would
  // grow the selection further without replacing).
  if (last_match_valid_ && last_match_start_ && last_match_end_) {
    auto a = buf->get_iter_at_mark(last_match_start_);
    auto b = buf->get_iter_at_mark(last_match_end_);
    if (matches_needle(a, b)) {
      replace_span(a.get_offset(), b.get_offset());
      return ReplaceResult::Replaced;
    }
  }

  if (opts.extend_selection && have_sel && !matches_needle(sel_a, sel_b)) {
    return ReplaceResult::Blocked;
  }

  if (find_match(opts, false)) {
    return ReplaceResult::Found;
  }
  return ReplaceResult::NotFound;
}

int MainWindow::replace_all(const FindOptions& opts) {
  if (opts.search_for.empty()) {
    return 0;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(opts, range_begin, range_end)) {
    return 0;
  }

  // One pass builds the replacement. One buffer edit is one undo step.
  // Walking GtkTextIter for every hit is itself a long stall, so a literal
  // search scans the bytes once. Entire-word and non-ASCII case folding
  // still use the iterator search, which knows those rules.
  const int start = range_begin.get_offset();
  const int end = range_end.get_offset();
  const Glib::ustring whole = buf->get_text();
  auto is_ascii = [](const Glib::ustring& text) {
    const char* data = text.data();
    const std::size_t n = text.bytes();
    for (std::size_t i = 0; i < n; ++i) {
      if (static_cast<unsigned char>(data[i]) >= 128) {
        return false;
      }
    }
    return true;
  };
  if (!opts.entire_word && (opts.case_sensitive ||
                            (is_ascii(opts.search_for) && is_ascii(opts.replace_with) &&
                             is_ascii(whole)))) {
    const Glib::ustring slice =
        (start == 0 && end == static_cast<int>(whole.length()))
            ? whole
            : whole.substr(static_cast<Glib::ustring::size_type>(start),
                           static_cast<Glib::ustring::size_type>(end - start));
    const std::string hay(slice.data(), slice.bytes());
    const std::string needle(opts.search_for.data(), opts.search_for.bytes());
    const std::string repl(opts.replace_with.data(), opts.replace_with.bytes());
    const std::string* scan_hay = &hay;
    const std::string* scan_needle = &needle;
    std::string folded_hay;
    std::string folded_needle;
    if (!opts.case_sensitive) {
      folded_hay.resize(hay.size());
      for (std::size_t i = 0; i < hay.size(); ++i) {
        folded_hay[i] = static_cast<char>(g_ascii_tolower(hay[i]));
      }
      folded_needle.resize(needle.size());
      for (std::size_t i = 0; i < needle.size(); ++i) {
        folded_needle[i] = static_cast<char>(g_ascii_tolower(needle[i]));
      }
      scan_hay = &folded_hay;
      scan_needle = &folded_needle;
    }
    std::string out;
    out.reserve(hay.size() + repl.size());
    int count = 0;
    std::size_t pos = 0;
    while (pos <= hay.size()) {
      const std::size_t found = scan_hay->find(*scan_needle, pos);
      if (found == std::string::npos) {
        out.append(hay, pos, std::string::npos);
        break;
      }
      out.append(hay, pos, found - pos);
      out.append(repl);
      pos = found + needle.size();
      ++count;
      if (needle.empty()) {
        break;
      }
    }
    if (count == 0) {
      return 0;
    }
    apply_bulk_replace(start, end, Glib::ustring(out));
    return count;
  }

  struct Hit {
    int start_off;
    int end_off;
  };
  std::vector<Hit> hits;
  Gtk::TextIter cursor = range_begin;
  Gtk::TextIter match_start, match_end;
  while (cursor.forward_search(opts.search_for, flags, match_start, match_end,
                               range_end)) {
    if (!opts.entire_word || is_entire_word(match_start, match_end)) {
      hits.push_back({match_start.get_offset(), match_end.get_offset()});
    }
    cursor = match_end;
    if (match_start == match_end) {
      if (!cursor.forward_char()) {
        break;
      }
    }
  }
  if (hits.empty()) {
    return 0;
  }

  Glib::ustring neu;
  neu.reserve(static_cast<std::size_t>(std::max(0, end - start)) +
              hits.size() * opts.replace_with.length());
  int at = start;
  for (const auto& hit : hits) {
    if (hit.start_off > at) {
      neu.append(whole.substr(
          static_cast<Glib::ustring::size_type>(at),
          static_cast<Glib::ustring::size_type>(hit.start_off - at)));
    }
    neu.append(opts.replace_with);
    at = hit.end_off;
  }
  if (at < end) {
    neu.append(whole.substr(static_cast<Glib::ustring::size_type>(at),
                            static_cast<Glib::ustring::size_type>(end - at)));
  }
  apply_bulk_replace(start, end, neu);
  return static_cast<int>(hits.size());
}

bool MainWindow::apply_bulk_replace(int start_off, int end_off,
                                    const Glib::ustring& neu) {
  auto buf = buffer();
  if (!buf) {
    return false;
  }
  const Glib::ustring old =
      buf->get_iter_at_offset(start_off).get_text(buf->get_iter_at_offset(end_off));
  const int old_nl = static_cast<int>(count_newlines(old.data(), old.bytes()));
  const int new_nl = static_cast<int>(count_newlines(neu.data(), neu.bytes()));
  const bool keep = !source_lines_.empty() && old_nl == new_nl;
  std::vector<char> kinds;
  if (keep) {
    snapshot_endings();
    kinds = ending_kinds();
  }
  buf->begin_user_action();
  const bool saved_restore = ending_restore_;
  if (keep) {
    ending_restore_ = true;
  }
  buf->erase(buf->get_iter_at_offset(start_off), buf->get_iter_at_offset(end_off));
  buf->insert(buf->get_iter_at_offset(start_off), neu);
  ending_restore_ = saved_restore;
  buf->end_user_action();
  if (keep && static_cast<int>(kinds.size()) == buf->get_line_count()) {
    remember_source_lines(buf->get_text(), kinds);
  } else if (!source_lines_.empty()) {
    source_lines_.clear();
    clear_ending_history();
  }
  clear_find_highlights();
  update_undo_redo_sensitivity();
  update_status();
  return true;
}

void MainWindow::on_find() {
  // Capture selection before the dialog takes focus so Search Selection Only
  // still has the user range after Find reselection. Clear the pin when Find
  // opens with no selection so a stale range is not reused.
  {
    auto buf = text_view_.get_buffer();
    Gtk::TextIter a, b;
    if (buf->get_selection_bounds(a, b) && a != b) {
      pin_selection_only_range();
    } else {
      clear_selection_only_range();
    }
  }
  clear_extend_anchor();

  FindReplaceDialog dlg(*this, find_opts_);
  dlg.signal_hide().connect([this]() { on_find_dialog_hidden(); });
  dlg.on_action = [this, &dlg](FindReplaceDialog::Action action,
                               const FindOptions& opts) -> bool {
    find_opts_ = opts;
    if (!opts.search_selection_only) {
      clear_selection_only_range();
    } else if (!sel_only_range_valid_) {
      pin_selection_only_range();
    }
    if (!opts.extend_selection) {
      clear_extend_anchor();
    }
    auto no_selection = [this, &dlg]() {
      Gtk::MessageDialog miss(dlg, "No text is selected.", false,
                              Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
      miss.set_secondary_text(
          "Search Selection Only needs a selection.");
      run_modal(app_, miss);
    };
    auto bounds_ok = [this, &opts]() {
      Gtk::TextIter begin, end;
      return get_search_bounds(opts, begin, end);
    };

    switch (action) {
      case FindReplaceDialog::Action::Find: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        FindOptions o = opts;
        const bool found = find_match(o, false);
        // Start at Top applies to this search only. The next Find in the
        // dialog continues from the cursor. F3 still forces the flag off.
        if (opts.start_at_top) {
          dlg.clear_start_at_top();
          find_opts_.start_at_top = false;
        }
        if (!found) {
          Gtk::MessageDialog miss(dlg, "Text not found.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(opts.search_for);
          run_modal(app_, miss);
          return false;
        }
        return true;
      }
      case FindReplaceDialog::Action::FindAll: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        // One capped scan, a chunk per idle, so Cancel stays responsive.
        // The selected hit follows Search Backwards.
        start_find_all(opts, &dlg);
        return true;
      }
      case FindReplaceDialog::Action::Replace: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        const ReplaceResult result = replace_current(opts);
        if (result == ReplaceResult::Blocked) {
          Gtk::MessageDialog miss(dlg, "Cannot replace this selection.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(
              "The selection extends past the match, and that match could "
              "not be replaced.");
          run_modal(app_, miss);
          return false;
        }
        if (result == ReplaceResult::NotFound) {
          Gtk::MessageDialog miss(dlg, "Text not found.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(opts.search_for);
          run_modal(app_, miss);
          return false;
        }
        return true;
      }
      case FindReplaceDialog::Action::ReplaceAll: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        // Whole-range replacement. Chunked so Cancel works, with a warning
        // when the grouped undo record would be huge.
        start_replace_all(opts, &dlg);
        return true;
      }
      default:
        return false;
    }
  };

  run_modal(app_, dlg);
  find_opts_ = dlg.options();
  // After first successful find, clear start_at_top for Find Next.
  find_opts_.start_at_top = false;
  // options() copies the checkbox, which would turn Extend back on after
  // the hide handler cleared it.
  find_opts_.extend_selection = false;
}

void MainWindow::on_find_next() {
  if (find_opts_.search_for.empty()) {
    on_find();
    return;
  }
  FindOptions o = find_opts_;
  o.start_at_top = false;
  o.extend_selection = find_opts_.extend_selection;
  if (o.search_selection_only) {
    // Pin whatever is selected now. A range remembered from the last Find
    // dialog is not still on screen. The match Find just selected is not
    // a range either: searching only that span finds the same hit again.
    clear_selection_only_range();
    auto buf = text_view_.get_buffer();
    Gtk::TextIter sel_a, sel_b;
    if (!buf || !buf->get_selection_bounds(sel_a, sel_b) || sel_a == sel_b) {
      last_notice_ = "No text is selected.";
      if (!test_mode()) {
        Gtk::MessageDialog miss(*this, "No text is selected.", false,
                                Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
        miss.set_secondary_text("Search Selection Only needs a selection.");
        run_modal(app_, miss);
      }
      return;
    }
    bool last_match_selected = false;
    if (last_match_valid_ && last_match_start_ && last_match_end_) {
      const auto match_start = buf->get_iter_at_mark(last_match_start_);
      const auto match_end = buf->get_iter_at_mark(last_match_end_);
      last_match_selected = sel_a.compare(match_start) == 0 &&
                            sel_b.compare(match_end) == 0;
    }
    if (last_match_selected) {
      o.search_selection_only = false;
    } else {
      pin_selection_only_range();
    }
  }
  if (!find_match(o, true)) {
    last_notice_ = "Text not found.";
    if (test_mode()) {
      return;
    }
    Gtk::MessageDialog miss(*this, "Text not found.", false, Gtk::MESSAGE_INFO,
                            Gtk::BUTTONS_OK, true);
    miss.set_secondary_text(find_opts_.search_for);
    run_modal(app_, miss);
  }
}

void MainWindow::on_go_to_line() {
  Gtk::Dialog dlg("Go to Line", *this, true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Go", Gtk::RESPONSE_OK);
  dlg.set_default_response(Gtk::RESPONSE_OK);

  auto* box = dlg.get_content_area();
  box->set_spacing(8);
  box->set_border_width(10);
  auto* label = Gtk::manage(new Gtk::Label("Line number:", true));
  label->set_halign(Gtk::ALIGN_START);
  auto* entry = Gtk::manage(new Gtk::Entry());
  entry->set_activates_default(true);
  auto buf = text_view_.get_buffer();
  auto iter = buf->get_iter_at_mark(buf->get_insert());
  entry->set_text(std::to_string(iter.get_line() + 1));
  box->pack_start(*label, Gtk::PACK_SHRINK);
  box->pack_start(*entry, Gtk::PACK_SHRINK);
  dlg.show_all();

  if (run_modal(app_, dlg) == Gtk::RESPONSE_OK) {
    int line = 0;
    if (!parse_go_to_line(entry->get_text().raw(), line)) {
      Gtk::MessageDialog bad(dlg, "Invalid line number.", false,
                             Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
      run_modal(app_, bad);
    } else {
      if (line < 1) {
        line = 1;
      }
      const int max_line = buf->get_line_count();
      if (line > max_line) {
        line = max_line;
      }
      auto dest = buf->get_iter_at_line(line - 1);
      buf->place_cursor(dest);
      text_view_.scroll_to(dest);
      update_status();
    }
  }
}

void MainWindow::on_toggle_wrap() {
  if (!wrap_item_) {
    return;
  }
  if (suppress_wrap_pref_) {
    text_view_.set_wrap_mode(wrap_item_->get_active() ? Gtk::WRAP_WORD_CHAR
                                                      : Gtk::WRAP_NONE);
    sync_long_line_window();
    return;
  }
  if (wrap_item_->get_active() && buffer_has_long_line()) {
    suppress_wrap_pref_ = true;
    wrap_item_->set_active(false);
    suppress_wrap_pref_ = false;
    text_view_.set_wrap_mode(Gtk::WRAP_NONE);
    sync_long_line_window();
    report_error(
        "Wrap stays off.",
        "A line is still too long to wrap. Wrap turns back on when that line is shorter.");
    return;
  }
  text_view_.set_wrap_mode(wrap_item_->get_active() ? Gtk::WRAP_WORD_CHAR
                                                    : Gtk::WRAP_NONE);
  app_.set_wrap_text(wrap_item_->get_active());
  sync_long_line_window();
  if (gutter_) {
    gutter_->queue_draw();
  }
}

void MainWindow::on_toggle_line_numbers() {
  if (!gutter_ || !line_numbers_item_) {
    return;
  }
  gutter_->set_visible_gutter(line_numbers_item_->get_active());
}

void MainWindow::apply_tab_width(int spaces) {
  tab_width_ = spaces;
  app_.set_tab_width(spaces);
  auto layout = text_view_.create_pango_layout(std::string(spaces, ' '));
  layout->set_font_description(font_desc_);
  int tw = 0, th = 0;
  layout->get_pixel_size(tw, th);
  Pango::TabArray tabs(1, true);
  tabs.set_tab(0, Pango::TAB_LEFT, tw);
  text_view_.set_tabs(tabs);
  text_view_.set_tab_width(static_cast<guint>(spaces));
  (void)th;
}

void MainWindow::on_tab_width() {
  Gtk::Dialog dlg("Tab Width", *this, true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_OK", Gtk::RESPONSE_OK);
  dlg.set_default_response(Gtk::RESPONSE_OK);

  auto* box = dlg.get_content_area();
  box->set_spacing(6);
  box->set_border_width(10);
  auto* label = Gtk::manage(new Gtk::Label("Spaces per tab:"));
  label->set_halign(Gtk::ALIGN_START);
  box->pack_start(*label, Gtk::PACK_SHRINK);

  Gtk::RadioButton::Group group;
  auto* r2 = Gtk::manage(new Gtk::RadioButton(group, "2"));
  auto* r4 = Gtk::manage(new Gtk::RadioButton(group, "4"));
  auto* r8 = Gtk::manage(new Gtk::RadioButton(group, "8"));
  if (tab_width_ == 2) {
    r2->set_active(true);
  } else if (tab_width_ == 8) {
    r8->set_active(true);
  } else {
    r4->set_active(true);
  }
  box->pack_start(*r2, Gtk::PACK_SHRINK);
  box->pack_start(*r4, Gtk::PACK_SHRINK);
  box->pack_start(*r8, Gtk::PACK_SHRINK);
  dlg.show_all();

  if (run_modal(app_, dlg) == Gtk::RESPONSE_OK) {
    int w = 4;
    if (r2->get_active()) {
      w = 2;
    } else if (r8->get_active()) {
      w = 8;
    }
    apply_tab_width(w);
  }
}

void MainWindow::install_editor_font(const Pango::FontDescription& desc) {
  if (!font_css_) {
    font_css_ = Gtk::CssProvider::create();
    text_view_.get_style_context()->add_provider(
        font_css_, GTK_STYLE_PROVIDER_PRIORITY_USER);
  }
  font_css_->load_from_data(editor_font_css(desc));

  auto buf = buffer();
  if (!buf) {
    return;
  }
  if (!font_tag_) {
    font_tag_ = buf->create_tag("lunduke-editor-font");
  }
  font_tag_->property_font_desc() = desc;
  apply_editor_font_tag();
}

void MainWindow::apply_editor_font_tag() {
  if (!font_tag_) {
    return;
  }
  auto buf = buffer();
  if (!buf || buf->begin() == buf->end()) {
    return;
  }
  const bool modified = buf->get_modified();
  buf->apply_tag(font_tag_, buf->begin(), buf->end());
  if (buf->get_modified() != modified) {
    buf->set_modified(modified);
  }
}

void MainWindow::on_font_tag_inserted(const Gtk::TextBuffer::iterator& pos,
                                     const Glib::ustring& text,
                                     int /*bytes*/) {
  // Runs after the default insert, so pos is the end of the new text.
  // Tag that range. A forward look tags the characters that were already
  // there when the insert repeats them (a duplicated first line, a
  // matching prefix, the same letters mid-line) and leaves the new
  // characters in the theme monospace.
  // Open and reload set the whole buffer after seeding_. Undo and redo
  // reinsert while ending_restore_ is set; those characters need the tag
  // too. GTK 3 tag-on toggles are right-gravity and there is no per-tag
  // gravity property, so an insert at offset 0 does not inherit the face
  // from the following text. This handler is what applies it.
  if (!font_tag_ || text.empty() || seeding_) {
    return;
  }
  auto buf = buffer();
  if (!buf) {
    return;
  }
  const bool modified = buf->get_modified();
  const int n = static_cast<int>(text.length());
  const int end_off = pos.get_offset();
  if (end_off >= n) {
    Gtk::TextIter start = pos;
    if (start.backward_chars(n)) {
      buf->apply_tag(font_tag_, start, pos);
    }
  }
  if (buf->get_modified() != modified) {
    buf->set_modified(modified);
  }
  sync_long_line_window();
}

void MainWindow::apply_font(const Pango::FontDescription& desc,
                            bool user_chosen) {
  font_desc_ = desc;
  font_user_chosen_ = user_chosen;
  app_.set_font(desc.to_string(), user_chosen);
  // The monospace style class forces the theme's monospace family. Use it
  // only when the user has not picked a face. The CSS provider and the
  // buffer tag carry the actual family and size. override_font does not
  // change GtkSourceView's layout on GTK 3.24.
  text_view_.set_monospace(!user_chosen);
  install_editor_font(desc);
  apply_tab_width(tab_width_);
  if (gutter_) {
    gutter_->refresh();
  }
}

void MainWindow::on_font() {
  Gtk::FontChooserDialog dlg("Font", *this);
  dlg.set_font_desc(font_desc_);
  if (run_modal(app_, dlg) == Gtk::RESPONSE_OK) {
    apply_font(dlg.get_font_desc(), true);
  }
}


void MainWindow::on_page_setup() {
  if (!print_settings_) {
    print_settings_ = Gtk::PrintSettings::create();
  }
  if (!page_setup_) {
    page_setup_ = Gtk::PageSetup::create();
  }
  app_.push_reentry();
  page_setup_ =
      Gtk::run_page_setup_dialog(*this, page_setup_, print_settings_);
  app_.pop_reentry();
}

int MainWindow::measure_print_tab(
    const Glib::RefPtr<Gtk::PrintContext>& context) const {
  auto layout = context->create_pango_layout();
  layout->set_font_description(font_desc_);
  const int spaces = std::max(1, tab_width_);
  layout->set_text(Glib::ustring(spaces, ' '));
  int width = 0;
  int height = 0;
  layout->get_size(width, height);
  (void)height;
  if (width < 1) {
    width = spaces * Pango::SCALE;
  }
  return width;
}

void MainWindow::configure_print_layout(
    const Glib::RefPtr<Pango::Layout>& layout,
    const Glib::RefPtr<Gtk::PrintContext>& context, int width_pango) {
  layout->set_font_description(font_desc_);
  layout->set_width(width_pango);
  layout->set_wrap(Pango::WRAP_WORD_CHAR);
  // Tab stops come from the print context, in pango units. The on-screen
  // tab array is in pixels and is the wrong width once the DPI changes.
  const int tab = measure_print_tab(context);
  Pango::TabArray tabs(1, false);
  tabs.set_tab(0, Pango::TAB_LEFT, tab);
  layout->set_tabs(tabs);
  last_print_tab_pos_ = tab;
  print_tabs_in_pixels_ = false;
}

int MainWindow::next_print_end(int offset) {
  auto buf = text_view_.get_buffer();
  if (!buf) {
    return offset;
  }
  const int total = buf->get_char_count();
  if (offset >= total) {
    return total;
  }
  auto iter = buf->get_iter_at_offset(offset);
  auto line_end = iter;
  if (!line_end.ends_line()) {
    line_end.forward_to_line_end();
  }
  int end = line_end.get_offset();
  if (end < total) {
    auto nl = buf->get_iter_at_offset(end);
    const gunichar ch = nl.get_char();
    if (ch == '\n' || ch == '\r') {
      ++end;
    }
  }
  if (end <= offset) {
    end = std::min(total, offset + 1);
  }
  return end;
}

double MainWindow::measure_print_chunk(
    const Glib::RefPtr<Gtk::PrintContext>& context, int width_pango, int start,
    int end, double min_height) {
  auto buf = text_view_.get_buffer();
  if (!buf || end <= start) {
    return min_height;
  }
  const Glib::ustring text = buf->get_iter_at_offset(start).get_text(
      buf->get_iter_at_offset(end));
  auto layout = context->create_pango_layout();
  configure_print_layout(layout, context, width_pango);
  layout->set_text(text);
  double height = 0.0;
  const int n_lines = layout->get_line_count();
  for (int i = 0; i < n_lines; ++i) {
    auto line = layout->get_line(i);
    Pango::Rectangle ink, logical;
    line->get_extents(ink, logical);
    height += static_cast<double>(logical.get_height()) / Pango::SCALE;
  }
  if (height < 1.0) {
    return min_height;
  }
  return height;
}

void MainWindow::draw_print_layout(const Cairo::RefPtr<Cairo::Context>& cr,
                                   const Glib::RefPtr<Pango::Layout>& layout,
                                   double& y, int row_begin, int row_end) const {
  if (!layout) {
    return;
  }
  // y is the top of the next line box. show_in_cairo_context draws on the
  // baseline; logical.y is that baseline relative to the top (usually
  // negative). Starting at y=0 clips the ascent on every page.
  // A long line keeps one layout and is drawn a visual-row slice at a time
  // so tab stops and wrapping do not restart at a character cut.
  const int n_lines = layout->get_line_count();
  if (row_begin < 0) {
    row_begin = 0;
  }
  if (row_end > n_lines) {
    row_end = n_lines;
  }
  for (int i = row_begin; i < row_end; ++i) {
    auto line = layout->get_line(i);
    Pango::Rectangle ink, logical;
    line->get_extents(ink, logical);
    const double line_height =
        static_cast<double>(logical.get_height()) / Pango::SCALE;
    const double baseline =
        y - static_cast<double>(logical.get_y()) / Pango::SCALE;
    cr->move_to(static_cast<double>(logical.get_x()) / Pango::SCALE, baseline);
    line->show_in_cairo_context(cr);
    y += line_height > 0.0 ? line_height : 1.0;
  }
}

void MainWindow::on_begin_print(
    const Glib::RefPtr<Gtk::PrintContext>& context) {
  print_page_breaks_.clear();
  print_pages_.clear();
  auto buf = buffer();
  if (!buf || !context) {
    return;
  }
  const int total = buf->get_char_count();
  const double page_height = std::max(1.0, context->get_height());
  const double page_width = std::max(1.0, context->get_width());
  const int width =
      static_cast<int>(std::floor(page_width * Pango::SCALE));

  double line_height = 12.0;
  {
    auto sample = context->create_pango_layout();
    configure_print_layout(sample, context, width);
    sample->set_text("Ag");
    Pango::Rectangle ink, logical;
    sample->get_extents(ink, logical);
    const double h = static_cast<double>(logical.get_height()) / Pango::SCALE;
    if (h > 1.0) {
      line_height = h;
    }
  }

  PrintPage page;
  Glib::ustring pending;
  // Height of slices already placed on this page. A tab or a wrapped line
  // flushes earlier lines into those slices. The next short lines have to
  // add their own height to this, or the page looks empty and keeps
  // accepting lines that then draw past the bottom.
  double committed = 0.0;
  double used = 0.0;
  bool page_empty = true;

  auto layout_rows_height = [&](const Glib::RefPtr<Pango::Layout>& layout,
                                int row_begin, int row_end) -> double {
    if (!layout) {
      return 0.0;
    }
    double height = 0.0;
    const int n = layout->get_line_count();
    if (row_begin < 0) {
      row_begin = 0;
    }
    if (row_end > n) {
      row_end = n;
    }
    for (int i = row_begin; i < row_end; ++i) {
      auto line = layout->get_line(i);
      if (!line) {
        continue;
      }
      Pango::Rectangle line_ink;
      Pango::Rectangle line_logical;
      line->get_extents(line_ink, line_logical);
      const double h =
          static_cast<double>(line_logical.get_height()) / Pango::SCALE;
      // A blank line can report a zero box. Count the font's line height
      // so a run of blank lines still fills the page.
      height += h > 0.0 ? h : line_height;
    }
    return height;
  };

  auto flush_pending = [&]() {
    if (pending.empty()) {
      return;
    }
    auto layout = context->create_pango_layout();
    configure_print_layout(layout, context, width);
    layout->set_text(pending);
    PrintSlice slice;
    slice.layout = layout;
    slice.row_begin = 0;
    slice.row_end = std::max(1, layout->get_line_count());
    committed += layout_rows_height(layout, slice.row_begin, slice.row_end);
    page.slices.push_back(std::move(slice));
    pending.clear();
    used = committed;
  };

  // Sum of the line boxes in one layout. That is what draw adds up.
  // A row measured on its own is a little shorter, and a page of those
  // rows runs past the bottom.
  auto block_height = [&](const Glib::ustring& block) -> double {
    if (block.empty()) {
      return 0.0;
    }
    auto layout = context->create_pango_layout();
    configure_print_layout(layout, context, width);
    layout->set_text(block);
    return layout_rows_height(layout, 0, layout->get_line_count());
  };

  // push the page that just filled. The break is where the next page starts.
  auto close_page = [&](int break_at) {
    flush_pending();
    if (page.slices.empty()) {
      used = 0.0;
      committed = 0.0;
      page_empty = true;
      return;
    }
    print_pages_.push_back(std::move(page));
    page = PrintPage{};
    print_page_breaks_.push_back(break_at);
    used = 0.0;
    committed = 0.0;
    page_empty = true;
  };

  int offset = 0;
  while (offset < total) {
    const int chunk_end = next_print_end(offset);
    if (chunk_end <= offset) {
      break;
    }
    const Glib::ustring text = buf->get_iter_at_offset(offset).get_text(
        buf->get_iter_at_offset(chunk_end));
    const int chars = chunk_end - offset;
    const bool has_tab = text.find('\t') != Glib::ustring::npos;
    // Measure the line with the print layout. A space-width guess calls a
    // run of W or fullwidth glyphs "simple" and reserves one row, then the
    // wrapped rows paint off the bottom of the page.
    int visual_rows = 1;
    double row_height = line_height;
    if (!has_tab && chars < kLongLineChars) {
      Glib::ustring probe = text;
      if (!probe.empty() && probe[probe.length() - 1] == '\n') {
        probe.erase(probe.length() - 1, 1);
      }
      if (!probe.empty() && probe[probe.length() - 1] == '\r') {
        probe.erase(probe.length() - 1, 1);
      }
      auto measured = context->create_pango_layout();
      configure_print_layout(measured, context, width);
      measured->set_text(probe);
      visual_rows = std::max(1, measured->get_line_count());
      if (visual_rows == 1) {
        Pango::Rectangle ink, logical;
        measured->get_extents(ink, logical);
        const double h =
            static_cast<double>(logical.get_height()) / Pango::SCALE;
        if (h > 0.0) {
          row_height = h;
        }
      }
    }
    const bool complex = has_tab || chars >= kLongLineChars || visual_rows > 1;

    if (!complex) {
      // Leave a few points, then compare the combined layout before the
      // page is closed. The per-line probe above still rejects a wrapped
      // run of wide glyphs. `committed` is the height already placed
      // (tabs, wraps, earlier blocks). The new block is added to that,
      // and the page closes before the sum passes the bottom.
      const double kPrintSlack = 4.0;
      const bool near_end =
          !page_empty && (used + row_height + kPrintSlack > page_height ||
                          used > page_height * 0.85);
      if (near_end) {
        const double total = committed + block_height(pending + text);
        if (total > page_height) {
          close_page(offset);
          pending.append(text);
          used = row_height;
        } else {
          pending.append(text);
          used = total;
        }
      } else {
        if (!page_empty && used + row_height > page_height) {
          close_page(offset);
        }
        pending.append(text);
        used += row_height;
      }
      page_empty = false;
      offset = chunk_end;
      continue;
    }

    flush_pending();
    auto layout = context->create_pango_layout();
    configure_print_layout(layout, context, width);
    layout->set_text(text);
    const int rows = std::max(1, layout->get_line_count());
    std::vector<double> heights(static_cast<std::size_t>(rows), line_height);
    for (int i = 0; i < rows; ++i) {
      auto line = layout->get_line(i);
      Pango::Rectangle ink, logical;
      line->get_extents(ink, logical);
      const double h =
          static_cast<double>(logical.get_height()) / Pango::SCALE;
      heights[static_cast<std::size_t>(i)] = h > 0.0 ? h : line_height;
    }

    int row = 0;
    while (row < rows) {
      if (!page_empty &&
          used + heights[static_cast<std::size_t>(row)] > page_height) {
        close_page(offset);
      }
      int row_end = row;
      double slice_h = 0.0;
      while (row_end < rows) {
        const double h = heights[static_cast<std::size_t>(row_end)];
        if (row_end > row &&
            ((page_empty && slice_h + h > page_height) ||
             (!page_empty && used + slice_h + h > page_height))) {
          break;
        }
        slice_h += h;
        ++row_end;
        if (page_empty && slice_h >= page_height) {
          break;
        }
      }
      if (row_end == row) {
        slice_h = heights[static_cast<std::size_t>(row)];
        row_end = row + 1;
      }
      PrintSlice slice;
      slice.layout = layout;
      slice.row_begin = row;
      slice.row_end = row_end;
      const double placed =
          layout_rows_height(layout, slice.row_begin, slice.row_end);
      page.slices.push_back(std::move(slice));
      committed += placed;
      used += placed;
      page_empty = false;
      row = row_end;
    }
    offset = chunk_end;
  }

  flush_pending();
  if (!page.slices.empty()) {
    print_pages_.push_back(std::move(page));
  }
  if (print_pages_.empty()) {
    print_pages_.push_back(PrintPage{});
  }
  // The last page has no following break.
  if (!print_page_breaks_.empty() &&
      print_page_breaks_.size() >= print_pages_.size()) {
    print_page_breaks_.resize(print_pages_.size() - 1);
  }
}

void MainWindow::on_draw_page(const Glib::RefPtr<Gtk::PrintContext>& context,
                              int page_nr) {
  if (!context || page_nr < 0 ||
      static_cast<std::size_t>(page_nr) >= print_pages_.size()) {
    return;
  }
  auto cr = context->get_cairo_context();
  cr->set_source_rgb(0.0, 0.0, 0.0);
  double y = 0.0;
  for (const auto& slice : print_pages_[static_cast<std::size_t>(page_nr)].slices) {
    draw_print_layout(cr, slice.layout, y, slice.row_begin, slice.row_end);
  }
}

void MainWindow::on_print() {
  if (!print_settings_) {
    print_settings_ = Gtk::PrintSettings::create();
  }
  if (!page_setup_) {
    page_setup_ = Gtk::PageSetup::create();
  }

  auto op = Gtk::PrintOperation::create();
  op->set_print_settings(print_settings_);
  op->set_default_page_setup(page_setup_);
  op->set_embed_page_setup(true);
  op->set_unit(Gtk::UNIT_POINTS);
  op->set_job_name(current_basename());
  op->set_allow_async(false);

  op->signal_begin_print().connect(
      [this, op](const Glib::RefPtr<Gtk::PrintContext>& context) {
        on_begin_print(context);
        const int n_pages = static_cast<int>(print_page_breaks_.size()) + 1;
        op->set_n_pages(std::max(1, n_pages));
      });
  op->signal_draw_page().connect(
      sigc::mem_fun(*this, &MainWindow::on_draw_page));

  try {
    app_.push_reentry();
    const auto result =
        op->run(Gtk::PRINT_OPERATION_ACTION_PRINT_DIALOG, *this);
    app_.pop_reentry();
    if (result == Gtk::PRINT_OPERATION_RESULT_APPLY) {
      print_settings_ = op->get_print_settings();
    }
  } catch (const Gtk::PrintError& error) {
    app_.pop_reentry();
    Gtk::MessageDialog err(*this, "Could not print.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    err.set_secondary_text(error.what());
    run_modal(app_, err);
  }

  print_page_breaks_.clear();
  print_pages_.clear();
}

void MainWindow::on_about() {
  Gtk::AboutDialog dlg;
  dlg.set_transient_for(*this);
  dlg.set_program_name("Lunduke Edit");
  dlg.set_version(kVersion);
  dlg.set_copyright("© 2026 The Lunduke Journal");
  dlg.set_website("https://lunduke.com");
  dlg.set_website_label("lunduke.com");
  dlg.set_license_type(Gtk::LICENSE_GPL_3_0);
  dlg.set_comments(
      "A light text editor for the Lunduke Computer Operating System.");
  dlg.set_logo_icon_name(resolve_app_icon_name());
  std::vector<Glib::ustring> authors{"The Lunduke Journal"};
  dlg.set_authors(authors);
  run_modal(app_, dlg);
}

}  // namespace lundukeedit
