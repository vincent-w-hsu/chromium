// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/profiles/profile_shortcut_manager_win.h"

#include <shlobj.h>  // For SHChangeNotify().
#include <stddef.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/stl_util.h"
#include "base/strings/string16.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/task_runner_util.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/win/shortcut.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_avatar_icon_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/shell_integration_win.h"
#include "chrome/browser/win/app_icon.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/chrome_unscaled_resources.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/installer/util/install_util.h"
#include "chrome/installer/util/shell_util.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_service.h"
#include "skia/ext/image_operations.h"
#include "skia/ext/platform_canvas.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/icon_util.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_family.h"

using content::BrowserThread;

namespace {

// Name of the badged icon file generated for a given profile.
const char kProfileIconFileName[] = "Google Profile.ico";

// Characters that are not allowed in Windows filenames. Taken from
// http://msdn.microsoft.com/en-us/library/aa365247.aspx
const base::char16 kReservedCharacters[] =
    L"<>:\"/\\|?*\x01\x02\x03\x04\x05"
    L"\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\x10\x11\x12\x13\x14\x15\x16\x17"
    L"\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F";

// The maximum number of characters allowed in profile shortcuts' file names.
// Warning: migration code will be needed if this is changed later, since
// existing shortcuts might no longer be found if the name is generated
// differently than it was when a shortcut was originally created.
const int kMaxProfileShortcutFileNameLength = 64;

// The avatar badge size needs to be half of the shortcut icon size because
// the Windows taskbar icon is 32x32 and the avatar icon overlay is 16x16. So to
// get the shortcut avatar badge and the avatar icon overlay to match up, we
// need to preserve those ratios when creating the shortcut icon.
const int kShortcutIconSize = 48;
const int kProfileAvatarBadgeSize = kShortcutIconSize / 2;

// Incrementing this number will cause profile icons to be regenerated on
// profile startup (it should be incremented whenever the product/avatar icons
// change, etc).
const int kCurrentProfileIconVersion = 5;

// 2x sized profile avatar icons. Mirrors |kDefaultAvatarIconResources| in
// profile_info_cache.cc.
const int kProfileAvatarIconResources2x[] = {
    IDR_PROFILE_AVATAR_2X_0,  IDR_PROFILE_AVATAR_2X_1,
    IDR_PROFILE_AVATAR_2X_2,  IDR_PROFILE_AVATAR_2X_3,
    IDR_PROFILE_AVATAR_2X_4,  IDR_PROFILE_AVATAR_2X_5,
    IDR_PROFILE_AVATAR_2X_6,  IDR_PROFILE_AVATAR_2X_7,
    IDR_PROFILE_AVATAR_2X_8,  IDR_PROFILE_AVATAR_2X_9,
    IDR_PROFILE_AVATAR_2X_10, IDR_PROFILE_AVATAR_2X_11,
    IDR_PROFILE_AVATAR_2X_12, IDR_PROFILE_AVATAR_2X_13,
    IDR_PROFILE_AVATAR_2X_14, IDR_PROFILE_AVATAR_2X_15,
    IDR_PROFILE_AVATAR_2X_16, IDR_PROFILE_AVATAR_2X_17,
    IDR_PROFILE_AVATAR_2X_18, IDR_PROFILE_AVATAR_2X_19,
    IDR_PROFILE_AVATAR_2X_20, IDR_PROFILE_AVATAR_2X_21,
    IDR_PROFILE_AVATAR_2X_22, IDR_PROFILE_AVATAR_2X_23,
    IDR_PROFILE_AVATAR_2X_24, IDR_PROFILE_AVATAR_2X_25,
    IDR_PROFILE_AVATAR_2X_26,
};

// Badges |app_icon_bitmap| with |avatar_bitmap| at the bottom right corner and
// returns the resulting SkBitmap.
SkBitmap BadgeIcon(const SkBitmap& app_icon_bitmap,
                   const SkBitmap& avatar_bitmap,
                   int scale_factor) {
  // TODO(dfried): This function often doesn't actually do the thing it claims
  // to. We should probably fix it.
  SkBitmap source_bitmap =
      profiles::GetAvatarIconAsSquare(avatar_bitmap, scale_factor);

  int avatar_badge_width = kProfileAvatarBadgeSize;
  if (app_icon_bitmap.width() != kShortcutIconSize) {
    avatar_badge_width =
        std::ceilf(app_icon_bitmap.width() *
                   (float{kProfileAvatarBadgeSize} / float{kShortcutIconSize}));
  }

  // Resize the avatar image down to the desired badge size, maintaining aspect
  // ratio (but prefer more square than rectangular when rounding).
  const int avatar_badge_height =
      std::ceilf(avatar_badge_width * (float{source_bitmap.height()} /
                                       float{source_bitmap.width()}));
  SkBitmap sk_icon = skia::ImageOperations::Resize(
      source_bitmap, skia::ImageOperations::RESIZE_LANCZOS3,
      avatar_badge_height, avatar_badge_width);

  // Sanity check - avatars shouldn't be taller than they are wide.
  DCHECK_GE(avatar_badge_width, avatar_badge_height);

  // Overlay the avatar on the icon, anchoring it to the bottom-right of the
  // icon.
  SkBitmap badged_bitmap;
  badged_bitmap.allocN32Pixels(app_icon_bitmap.width(),
                               app_icon_bitmap.height());
  SkCanvas offscreen_canvas(badged_bitmap);
  offscreen_canvas.clear(SK_ColorTRANSPARENT);
  offscreen_canvas.drawBitmap(app_icon_bitmap, 0, 0);

  // Render the avatar in a cutout circle. If the avatar is not square, center
  // it in the circle but favor pushing it further down.
  const int cutout_size = avatar_badge_width;
  const int cutout_left = app_icon_bitmap.width() - cutout_size;
  const int cutout_top = app_icon_bitmap.height() - cutout_size;
  const int icon_left = cutout_left;
  const int icon_top =
      cutout_top + int{std::ceilf((cutout_size - avatar_badge_height) / 2.0f)};
  const SkRRect clip_circle = SkRRect::MakeOval(
      SkRect::MakeXYWH(cutout_left, cutout_top, cutout_size, cutout_size));

  offscreen_canvas.clipRRect(clip_circle, true);
  offscreen_canvas.drawBitmap(sk_icon, icon_left, icon_top);
  return badged_bitmap;
}

// Updates the preferences with the current icon version on icon creation
// success.
void OnProfileIconCreateSuccess(base::FilePath profile_path) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!g_browser_process->profile_manager())
    return;
  Profile* profile =
      g_browser_process->profile_manager()->GetProfileByPath(profile_path);
  if (profile) {
    profile->GetPrefs()->SetInteger(prefs::kProfileIconVersion,
                                    kCurrentProfileIconVersion);
  }
}

// Creates a desktop shortcut icon file (.ico) on the disk for a given profile,
// badging the icon with the profile avatar.
// Returns a path to the shortcut icon file on disk, which is empty if this
// fails. Use index 0 when assigning the resulting file as the icon. If both
// given bitmaps are empty, an unbadged icon is created.
// Returns the path to the created icon on success and an empty base::FilePath
// on failure.
// TODO(calamity): Ideally we'd just copy the app icon verbatim from the exe's
// resources in the case of an unbadged icon.
base::FilePath CreateOrUpdateShortcutIconForProfile(
    const base::FilePath& profile_path,
    const SkBitmap& avatar_bitmap_1x,
    const SkBitmap& avatar_bitmap_2x) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  if (!base::PathExists(profile_path))
    return base::FilePath();

  std::unique_ptr<gfx::ImageFamily> family = GetAppIconImageFamily();
  if (!family)
    return base::FilePath();

  // TODO(mgiuca): A better approach would be to badge each image in the
  // ImageFamily (scaling the badge to the correct size), and then re-export the
  // family (as opposed to making a family with just 48 and 256, then scaling
  // those images to about a dozen different sizes).
  SkBitmap app_icon_bitmap =
      family->CreateExact(kShortcutIconSize, kShortcutIconSize).AsBitmap();
  if (app_icon_bitmap.isNull())
    return base::FilePath();

  gfx::ImageFamily badged_bitmaps;
  if (!avatar_bitmap_1x.empty()) {
    badged_bitmaps.Add(gfx::Image::CreateFrom1xBitmap(
        BadgeIcon(app_icon_bitmap, avatar_bitmap_1x, 1)));
  }

  SkBitmap large_app_icon_bitmap =
      family->CreateExact(IconUtil::kLargeIconSize, IconUtil::kLargeIconSize)
          .AsBitmap();
  if (!large_app_icon_bitmap.isNull() && !avatar_bitmap_2x.empty()) {
    badged_bitmaps.Add(gfx::Image::CreateFrom1xBitmap(
        BadgeIcon(large_app_icon_bitmap, avatar_bitmap_2x, 2)));
  }

  // If we have no badged bitmaps, we should just use the default chrome icon.
  if (badged_bitmaps.empty()) {
    badged_bitmaps.Add(gfx::Image::CreateFrom1xBitmap(app_icon_bitmap));
    if (!large_app_icon_bitmap.isNull()) {
      badged_bitmaps.Add(gfx::Image::CreateFrom1xBitmap(large_app_icon_bitmap));
    }
  }
  // Finally, write the .ico file containing this new bitmap.
  const base::FilePath icon_path =
      profiles::internal::GetProfileIconPath(profile_path);
  const bool had_icon = base::PathExists(icon_path);

  if (!IconUtil::CreateIconFileFromImageFamily(badged_bitmaps, icon_path)) {
    // This can happen if the profile directory is deleted between the beginning
    // of this function and here.
    return base::FilePath();
  }

  if (had_icon) {
    // This invalidates the Windows icon cache and causes the icon changes to
    // register with the taskbar and desktop. SHCNE_ASSOCCHANGED will cause a
    // desktop flash and we would like to avoid that if possible.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
  } else {
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATH, icon_path.value().c_str(), NULL);
  }
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::UI},
      base::BindOnce(&OnProfileIconCreateSuccess, profile_path));
  return icon_path;
}

// Gets the user and system directories for desktop shortcuts. Parameters may
// be NULL if a directory type is not needed. Returns true on success.
bool GetDesktopShortcutsDirectories(
    base::FilePath* user_shortcuts_directory,
    base::FilePath* system_shortcuts_directory) {
  if (user_shortcuts_directory &&
      !ShellUtil::GetShortcutPath(ShellUtil::SHORTCUT_LOCATION_DESKTOP,
                                  ShellUtil::CURRENT_USER,
                                  user_shortcuts_directory)) {
    NOTREACHED();
    return false;
  }
  if (system_shortcuts_directory &&
      !ShellUtil::GetShortcutPath(ShellUtil::SHORTCUT_LOCATION_DESKTOP,
                                  ShellUtil::SYSTEM_LEVEL,
                                  system_shortcuts_directory)) {
    NOTREACHED();
    return false;
  }
  return true;
}

// Returns the long form of |path|, which will expand any shortened components
// like "foo~2" to their full names.
base::FilePath ConvertToLongPath(const base::FilePath& path) {
  const size_t length = GetLongPathName(path.value().c_str(), NULL, 0);
  if (length != 0 && length != path.value().length()) {
    std::vector<wchar_t> long_path(length);
    if (GetLongPathName(path.value().c_str(), &long_path[0], length) != 0)
      return base::FilePath(&long_path[0]);
  }
  return path;
}

// Returns true if the file at |path| is a Chrome shortcut and returns its
// command line in output parameter |command_line|.
bool IsChromeShortcut(const base::FilePath& path,
                      const base::FilePath& chrome_exe,
                      base::string16* command_line) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  if (path.Extension() != installer::kLnkExt)
    return false;

  base::FilePath target_path;
  if (!base::win::ResolveShortcut(path, &target_path, command_line))
    return false;
  // One of the paths may be in short (elided) form. Compare long paths to
  // ensure these are still properly matched.
  return ConvertToLongPath(target_path) == ConvertToLongPath(chrome_exe);
}

// A functor checks if |path| is the Chrome desktop shortcut (|chrome_exe|)
// that have the specified |command_line|. If |include_empty_command_lines| is
// true Chrome desktop shortcuts with empty command lines will also be included.
struct ChromeCommandLineFilter {
  const base::FilePath& chrome_exe;
  const base::string16& command_line;
  bool include_empty_command_lines;

  ChromeCommandLineFilter(const base::FilePath& chrome_exe,
                          const base::string16& command_line,
                          bool include_empty_command_lines)
      : chrome_exe(chrome_exe),
        command_line(command_line),
        include_empty_command_lines(include_empty_command_lines) {}

  bool operator()(const base::FilePath& path) const {
    base::string16 shortcut_command_line;
    if (!IsChromeShortcut(path, chrome_exe, &shortcut_command_line))
      return false;

    // TODO(asvitkine): Change this to build a CommandLine object and ensure all
    // args from |command_line| are present in the shortcut's CommandLine. This
    // will be more robust when |command_line| contains multiple args.
    if ((shortcut_command_line.empty() && include_empty_command_lines) ||
        (shortcut_command_line.find(command_line) != base::string16::npos)) {
      return true;
    }
    return false;
  }
};

// Get the file paths of desktop files and folders optionally filtered
// by |filter|.
std::set<base::FilePath> ListUserDesktopContents(
    const ChromeCommandLineFilter* filter) {
  std::set<base::FilePath> result;

  base::FilePath user_shortcuts_directory;
  if (!GetDesktopShortcutsDirectories(&user_shortcuts_directory, nullptr))
    return result;

  base::FileEnumerator enumerator(
      user_shortcuts_directory, false,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    if (!filter || (*filter)(path))
      result.insert(path);
  }
  return result;
}

// Renames the given desktop shortcut and informs the shell of this change.
bool RenameDesktopShortcut(const base::FilePath& old_shortcut_path,
                           const base::FilePath& new_shortcut_path) {
  if (!base::Move(old_shortcut_path, new_shortcut_path))
    return false;

  // Notify the shell of the rename, which allows the icon to keep its position
  // on the desktop when renamed. Note: This only works if either SHCNF_FLUSH or
  // SHCNF_FLUSHNOWAIT is specified as a flag.
  SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATH | SHCNF_FLUSHNOWAIT,
                 old_shortcut_path.value().c_str(),
                 new_shortcut_path.value().c_str());
  return true;
}

// Renames an existing Chrome desktop profile shortcut.
// |profile_shortcuts| are Chrome desktop shortcuts for the profile (there can
// be several).
// |desktop_contents| is the collection of all user desktop shortcuts
// (not only Chrome). It is used to make an unique shortcut for the
// |new_profile_name| among all shortcuts.
// This function updates |profile_shortcuts| and |desktop_contents| respectively
// when renaming occurs.
void RenameChromeDesktopShortcutForProfile(
    const base::string16& old_profile_name,
    const base::string16& new_profile_name,
    std::set<base::FilePath>* profile_shortcuts,
    std::set<base::FilePath>* desktop_contents) {
  DCHECK(profile_shortcuts);
  DCHECK(desktop_contents);
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  base::FilePath user_shortcuts_directory;
  base::FilePath system_shortcuts_directory;
  if (!GetDesktopShortcutsDirectories(&user_shortcuts_directory,
                                      &system_shortcuts_directory)) {
    return;
  }

  // Get a new unique shortcut name.
  const base::string16 new_shortcut_filename =
      profiles::internal::GetUniqueShortcutFilenameForProfile(
          new_profile_name, *desktop_contents);
  const base::FilePath new_shortcut_path =
      user_shortcuts_directory.Append(new_shortcut_filename);

  if (!profile_shortcuts->empty()) {
    // From all profile_shortcuts choose the one with a known (canonical) name.
    profiles::internal::ShortcutFilenameMatcher matcher(old_profile_name);
    auto it = std::find_if(profile_shortcuts->begin(), profile_shortcuts->end(),
                           [&matcher](const base::FilePath& p) {
                             return matcher.IsCanonical(p.BaseName().value());
                           });
    // If all profile_shortcuts were renamed by user, respect it and do not
    // rename.
    if (it == profile_shortcuts->end())
      return;
    const base::FilePath old_shortcut_path = *it;

    // Rename the old shortcut unless a system-level shortcut exists at the
    // destination, in which case the old shortcut is simply deleted.
    const base::FilePath possible_new_system_shortcut =
        system_shortcuts_directory.Append(new_shortcut_filename);
    if (base::PathExists(possible_new_system_shortcut)) {
      if (base::DeleteFile(old_shortcut_path, false)) {
        profile_shortcuts->erase(old_shortcut_path);
        desktop_contents->erase(old_shortcut_path);
      } else {
        DLOG(ERROR) << "Could not delete Windows profile desktop shortcut.";
      }
    } else {
      if (RenameDesktopShortcut(old_shortcut_path, new_shortcut_path)) {
        profile_shortcuts->erase(old_shortcut_path);
        desktop_contents->erase(old_shortcut_path);
        profile_shortcuts->insert(new_shortcut_path);
        desktop_contents->insert(new_shortcut_path);
      } else {
        DLOG(ERROR) << "Could not rename Windows profile desktop shortcut.";
      }
    }
  } else {
    // If the shortcut does not exist, it may have been deleted by the user.
    // It's also possible that a system-level shortcut exists instead - this
    // should only be the case for the original Chrome shortcut from an
    // installation. If that's the case, copy that one over - it will get its
    // properties updated by
    // |CreateOrUpdateDesktopShortcutsAndIconForProfile()|.
    const auto old_shortcut_filename =
        profiles::internal::GetShortcutFilenameForProfile(old_profile_name);
    const base::FilePath possible_old_system_shortcut =
        system_shortcuts_directory.Append(old_shortcut_filename);
    if (base::PathExists(possible_old_system_shortcut)) {
      if (base::CopyFile(possible_old_system_shortcut, new_shortcut_path)) {
        profile_shortcuts->insert(new_shortcut_path);
        desktop_contents->insert(new_shortcut_path);
      } else {
        DLOG(ERROR) << "Could not copy Windows profile desktop shortcut.";
      }
    }
  }
}

struct CreateOrUpdateShortcutsParams {
  CreateOrUpdateShortcutsParams(
      base::FilePath profile_path,
      ProfileShortcutManagerWin::CreateOrUpdateMode create_mode,
      ProfileShortcutManagerWin::NonProfileShortcutAction action)
      : create_mode(create_mode), action(action), profile_path(profile_path) {}
  ~CreateOrUpdateShortcutsParams() {}

  ProfileShortcutManagerWin::CreateOrUpdateMode create_mode;
  ProfileShortcutManagerWin::NonProfileShortcutAction action;

  // The path for this profile.
  base::FilePath profile_path;
  // The profile name before this update. Empty on create.
  base::string16 old_profile_name;
  // The new profile name.
  base::string16 profile_name;
  // Avatar images for this profile.
  SkBitmap avatar_image_1x;
  SkBitmap avatar_image_2x;
};

// Updates all desktop shortcuts for the given profile to have the specified
// parameters. If |params.create_mode| is CREATE_WHEN_NONE_FOUND, a new shortcut
// is created if no existing ones were found. Whether non-profile shortcuts
// should be updated is specified by |params.action|. File and COM operations
// must be allowed on the calling thread.
void CreateOrUpdateDesktopShortcutsAndIconForProfile(
    const CreateOrUpdateShortcutsParams& params) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  const base::FilePath shortcut_icon = CreateOrUpdateShortcutIconForProfile(
      params.profile_path, params.avatar_image_1x, params.avatar_image_2x);
  if (shortcut_icon.empty() ||
      params.create_mode ==
          ProfileShortcutManagerWin::CREATE_OR_UPDATE_ICON_ONLY) {
    return;
  }

  base::FilePath chrome_exe;
  if (!base::PathService::Get(base::FILE_EXE, &chrome_exe)) {
    NOTREACHED();
    return;
  }

  std::set<base::FilePath> desktop_contents = ListUserDesktopContents(nullptr);

  const base::string16 command_line =
      profiles::internal::CreateProfileShortcutFlags(params.profile_path);
  ChromeCommandLineFilter filter(
      chrome_exe, command_line,
      params.action == ProfileShortcutManagerWin::UPDATE_NON_PROFILE_SHORTCUTS);

  std::set<base::FilePath> shortcuts;
  // Do not call ListUserDesktopContents again (but with filter) to avoid
  // excess work inside it. Just reuse non-filtered desktop_contents.
  // We need both of them (desktop_contents and shortcuts) later.
  std::copy_if(desktop_contents.begin(), desktop_contents.end(),
               std::inserter(shortcuts, shortcuts.begin()), filter);

  if (params.old_profile_name != params.profile_name) {
    RenameChromeDesktopShortcutForProfile(params.old_profile_name,
                                          params.profile_name, &shortcuts,
                                          &desktop_contents);
  }

  ShellUtil::ShortcutProperties properties(ShellUtil::CURRENT_USER);
  ShellUtil::AddDefaultShortcutProperties(chrome_exe, &properties);

  // Only set the profile-specific properties when |profile_name| is non empty.
  // If it is empty, it means the shortcut being created should be a regular,
  // non-profile Chrome shortcut.
  if (!params.profile_name.empty()) {
    properties.set_arguments(command_line);
    properties.set_icon(shortcut_icon, 0);
  } else {
    // Set the arguments explicitly to the empty string to ensure that
    // |ShellUtil::CreateOrUpdateShortcut| updates that part of the shortcut.
    properties.set_arguments(base::string16());
  }

  properties.set_app_id(shell_integration::win::GetChromiumModelIdForProfile(
      params.profile_path));

  ShellUtil::ShortcutOperation operation =
      ShellUtil::SHELL_SHORTCUT_REPLACE_EXISTING;

  if (params.create_mode == ProfileShortcutManagerWin::CREATE_WHEN_NONE_FOUND &&
      shortcuts.empty()) {
    const base::string16 shortcut_name =
        profiles::internal::GetUniqueShortcutFilenameForProfile(
            params.profile_name, desktop_contents);
    shortcuts.insert(base::FilePath(shortcut_name));
    operation = ShellUtil::SHELL_SHORTCUT_CREATE_IF_NO_SYSTEM_LEVEL;
  }

  for (const auto& shortcut : shortcuts) {
    const base::FilePath shortcut_name = shortcut.BaseName().RemoveExtension();
    properties.set_shortcut_name(shortcut_name.value());
    ShellUtil::CreateOrUpdateShortcut(ShellUtil::SHORTCUT_LOCATION_DESKTOP,
                                      properties, operation);
  }
}

// Returns true if any desktop shortcuts exist with target |chrome_exe|,
// regardless of their command line arguments.
bool ChromeDesktopShortcutsExist(const base::FilePath& chrome_exe) {
  base::FilePath user_shortcuts_directory;
  if (!GetDesktopShortcutsDirectories(&user_shortcuts_directory, NULL))
    return false;

  base::FileEnumerator enumerator(user_shortcuts_directory, false,
                                  base::FileEnumerator::FILES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    if (IsChromeShortcut(path, chrome_exe, NULL))
      return true;
  }

  return false;
}

// Deletes all desktop shortcuts for the specified profile. If
// |ensure_shortcuts_remain| is true, then a regular non-profile shortcut will
// be created if this function would otherwise delete the last Chrome desktop
// shortcut(s). File and COM operations must be allowed on the calling thread.
void DeleteDesktopShortcuts(const base::FilePath& profile_path,
                            bool ensure_shortcuts_remain) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  base::FilePath chrome_exe;
  if (!base::PathService::Get(base::FILE_EXE, &chrome_exe)) {
    NOTREACHED();
    return;
  }

  const base::string16 command_line =
      profiles::internal::CreateProfileShortcutFlags(profile_path);
  ChromeCommandLineFilter filter(chrome_exe, command_line, false);
  const std::set<base::FilePath> shortcuts = ListUserDesktopContents(&filter);

  for (const auto& shortcut : shortcuts) {
    // Use base::DeleteFile() instead of ShellUtil::RemoveShortcuts(), as the
    // latter causes non-profile taskbar shortcuts to be removed since it
    // doesn't consider the command-line of the shortcuts it deletes.
    // TODO(huangs): Refactor with ShellUtil::RemoveShortcuts().
    base::win::UnpinShortcutFromTaskbar(shortcut);
    base::DeleteFile(shortcut, false);
    // Notify the shell that the shortcut was deleted to ensure desktop refresh.
    SHChangeNotify(SHCNE_DELETE, SHCNF_PATH, shortcut.value().c_str(), nullptr);
  }

  // If |ensure_shortcuts_remain| is true and deleting this profile caused the
  // last shortcuts to be removed, re-create a regular non-profile shortcut.
  const bool had_shortcuts = !shortcuts.empty();
  if (ensure_shortcuts_remain && had_shortcuts &&
      !ChromeDesktopShortcutsExist(chrome_exe)) {
    ShellUtil::ShortcutProperties properties(ShellUtil::CURRENT_USER);
    ShellUtil::AddDefaultShortcutProperties(chrome_exe, &properties);
    properties.set_shortcut_name(
        profiles::internal::GetShortcutFilenameForProfile(base::string16()));
    ShellUtil::CreateOrUpdateShortcut(
        ShellUtil::SHORTCUT_LOCATION_DESKTOP, properties,
        ShellUtil::SHELL_SHORTCUT_CREATE_IF_NO_SYSTEM_LEVEL);
  }
}

// Returns true if profile at |profile_path| has any shortcuts. Does not
// consider non-profile shortcuts. File and COM operations must be allowed on
// the calling thread.
bool HasAnyProfileShortcuts(const base::FilePath& profile_path) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  base::FilePath chrome_exe;
  if (!base::PathService::Get(base::FILE_EXE, &chrome_exe)) {
    NOTREACHED();
    return false;
  }

  const base::string16 command_line =
      profiles::internal::CreateProfileShortcutFlags(profile_path);
  ChromeCommandLineFilter filter(chrome_exe, command_line, false);
  return !ListUserDesktopContents(&filter).empty();
}

// Replaces any reserved characters with spaces, and trims the resulting string
// to prevent any leading and trailing spaces. Also makes sure that the
// resulting filename doesn't exceed |kMaxProfileShortcutFileNameLength|.
// TODO(macourteau): find a way to limit the total path's length to MAX_PATH
// instead of limiting the profile's name to |kMaxProfileShortcutFileNameLength|
// characters.
base::string16 SanitizeShortcutProfileNameString(
    const base::string16& profile_name) {
  base::string16 sanitized = profile_name;
  size_t pos = sanitized.find_first_of(kReservedCharacters);
  while (pos != base::string16::npos) {
    sanitized[pos] = L' ';
    pos = sanitized.find_first_of(kReservedCharacters, pos + 1);
  }

  base::TrimWhitespace(sanitized, base::TRIM_LEADING, &sanitized);
  if (sanitized.size() > kMaxProfileShortcutFileNameLength)
    sanitized.erase(kMaxProfileShortcutFileNameLength);
  base::TrimWhitespace(sanitized, base::TRIM_TRAILING, &sanitized);

  return sanitized;
}

// Returns a copied SkBitmap for the given image that can be safely passed to
// another thread.
SkBitmap GetSkBitmapCopy(const gfx::Image& image) {
  DCHECK(!image.IsEmpty());
  const SkBitmap* image_bitmap = image.ToSkBitmap();
  SkBitmap bitmap_copy;
  if (bitmap_copy.tryAllocPixels(image_bitmap->info()))
    image_bitmap->readPixels(bitmap_copy.info(), bitmap_copy.getPixels(),
                             bitmap_copy.rowBytes(), 0, 0);
  return bitmap_copy;
}

// Returns a copied SkBitmap for the given resource id that can be safely passed
// to another thread.
SkBitmap GetImageResourceSkBitmapCopy(int resource_id) {
  const gfx::Image image =
      ui::ResourceBundle::GetSharedInstance().GetNativeImageNamed(resource_id);
  return GetSkBitmapCopy(image);
}

}  // namespace

namespace profiles {
namespace internal {

base::FilePath GetProfileIconPath(const base::FilePath& profile_path) {
  return profile_path.AppendASCII(kProfileIconFileName);
}

base::string16 GetShortcutFilenameForProfile(
    const base::string16& profile_name) {
  base::string16 shortcut_name;
  if (!profile_name.empty()) {
    shortcut_name.append(SanitizeShortcutProfileNameString(profile_name));
    shortcut_name.append(L" - ");
    shortcut_name.append(l10n_util::GetStringUTF16(IDS_SHORT_PRODUCT_NAME));
  } else {
    shortcut_name.append(InstallUtil::GetShortcutName());
  }
  return shortcut_name + installer::kLnkExt;
}

base::string16 GetUniqueShortcutFilenameForProfile(
    const base::string16& profile_name,
    const std::set<base::FilePath>& excludes) {
  std::set<base::string16> excludes_names;
  std::transform(excludes.begin(), excludes.end(),
                 std::inserter(excludes_names, excludes_names.begin()),
                 [](const base::FilePath& e) { return e.BaseName().value(); });

  const auto base_name = GetShortcutFilenameForProfile(profile_name);
  auto name = base_name;
  const base::FilePath base_path(base_name);
  for (int uniquifier = 1; excludes_names.count(name) > 0; ++uniquifier) {
    const auto suffix = base::StringPrintf(" (%d)", uniquifier);
    name = base_path.InsertBeforeExtensionASCII(suffix).value();
  }
  return name;
}

// Corresponds to GetUniqueShortcutFilenameForProfile.
ShortcutFilenameMatcher::ShortcutFilenameMatcher(
    const base::string16& profile_name)
    : profile_shortcut_filename_(GetShortcutFilenameForProfile(profile_name)),
      lnk_ext_(installer::kLnkExt),
      profile_shortcut_name_(profile_shortcut_filename_) {
  DCHECK(profile_shortcut_name_.ends_with(lnk_ext_));
  profile_shortcut_name_.remove_suffix(lnk_ext_.size());
}

bool ShortcutFilenameMatcher::IsCanonical(
    const base::string16& filename) const {
  if (filename == profile_shortcut_filename_)
    return true;

  base::StringPiece16 shortcut_suffix(filename);
  if (!shortcut_suffix.starts_with(profile_shortcut_name_))
    return false;
  shortcut_suffix.remove_prefix(profile_shortcut_name_.size());

  if (!shortcut_suffix.ends_with(lnk_ext_))
    return false;
  shortcut_suffix.remove_suffix(lnk_ext_.size());

  if (shortcut_suffix.size() < 4 || !shortcut_suffix.starts_with(L" (") ||
      !shortcut_suffix.ends_with(L")")) {
    return false;
  }
  return std::all_of(shortcut_suffix.begin() + 2, shortcut_suffix.end() - 1,
                     iswdigit);
}

base::string16 CreateProfileShortcutFlags(const base::FilePath& profile_path) {
  return base::StringPrintf(
      L"--%ls=\"%ls\"", base::ASCIIToUTF16(switches::kProfileDirectory).c_str(),
      profile_path.BaseName().value().c_str());
}

}  // namespace internal
}  // namespace profiles

namespace {
bool disabled_for_unit_tests = false;
}

void ProfileShortcutManager::DisableForUnitTests() {
  disabled_for_unit_tests = true;
}

// static
bool ProfileShortcutManager::IsFeatureEnabled() {
  if (disabled_for_unit_tests)
    return false;

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kEnableProfileShortcutManager))
    return true;

  base::FilePath user_data_dir;
  bool success = base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir);
  DCHECK(success);
  base::FilePath default_user_data_dir;
  success = chrome::GetDefaultUserDataDirectory(&default_user_data_dir);
  DCHECK(success);
  return user_data_dir == default_user_data_dir;
}

// static
std::unique_ptr<ProfileShortcutManager> ProfileShortcutManager::Create(
    ProfileManager* manager) {
  return std::make_unique<ProfileShortcutManagerWin>(manager);
}

ProfileShortcutManagerWin::ProfileShortcutManagerWin(ProfileManager* manager)
    : profile_manager_(manager) {
  DCHECK_EQ(base::size(kProfileAvatarIconResources2x),
            profiles::GetDefaultAvatarIconCount());

  registrar_.Add(this, chrome::NOTIFICATION_PROFILE_CREATED,
                 content::NotificationService::AllSources());

  profile_manager_->GetProfileAttributesStorage().AddObserver(this);
}

ProfileShortcutManagerWin::~ProfileShortcutManagerWin() {
  profile_manager_->GetProfileAttributesStorage().RemoveObserver(this);
}

void ProfileShortcutManagerWin::CreateOrUpdateProfileIcon(
    const base::FilePath& profile_path) {
  CreateOrUpdateShortcutsForProfileAtPath(
      profile_path, CREATE_OR_UPDATE_ICON_ONLY, IGNORE_NON_PROFILE_SHORTCUTS);
}

void ProfileShortcutManagerWin::CreateProfileShortcut(
    const base::FilePath& profile_path) {
  CreateOrUpdateShortcutsForProfileAtPath(profile_path, CREATE_WHEN_NONE_FOUND,
                                          IGNORE_NON_PROFILE_SHORTCUTS);
}

void ProfileShortcutManagerWin::RemoveProfileShortcuts(
    const base::FilePath& profile_path) {
  base::CreateCOMSTATaskRunnerWithTraits({base::MayBlock()})
      ->PostTask(FROM_HERE,
                 base::BindOnce(&DeleteDesktopShortcuts, profile_path, false));
}

void ProfileShortcutManagerWin::HasProfileShortcuts(
    const base::FilePath& profile_path,
    const base::Callback<void(bool)>& callback) {
  base::PostTaskAndReplyWithResult(
      base::CreateCOMSTATaskRunnerWithTraits({base::MayBlock()}).get(),
      FROM_HERE, base::Bind(&HasAnyProfileShortcuts, profile_path), callback);
}

void ProfileShortcutManagerWin::GetShortcutProperties(
    const base::FilePath& profile_path,
    base::CommandLine* command_line,
    base::string16* name,
    base::FilePath* icon_path) {
  base::FilePath chrome_exe;
  if (!base::PathService::Get(base::FILE_EXE, &chrome_exe)) {
    NOTREACHED();
    return;
  }

  ProfileAttributesStorage& storage =
      profile_manager_->GetProfileAttributesStorage();
  ProfileAttributesEntry* entry;
  bool has_entry = storage.GetProfileAttributesWithPath(profile_path, &entry);
  DCHECK(has_entry);

  // The used profile name should be empty if there is only 1 profile.
  base::string16 shortcut_profile_name;
  if (storage.GetNumberOfProfiles() > 1u)
    shortcut_profile_name = entry->GetName();

  *name = base::FilePath(profiles::internal::GetShortcutFilenameForProfile(
                             shortcut_profile_name))
              .RemoveExtension()
              .value();

  command_line->ParseFromString(
      L"\"" + chrome_exe.value() + L"\" " +
      profiles::internal::CreateProfileShortcutFlags(profile_path));

  *icon_path = profiles::internal::GetProfileIconPath(profile_path);
}

void ProfileShortcutManagerWin::OnProfileAdded(
    const base::FilePath& profile_path) {
  CreateOrUpdateProfileIcon(profile_path);
  if (profile_manager_->GetProfileAttributesStorage().GetNumberOfProfiles() ==
      2u) {
    // When the second profile is added, make existing non-profile shortcuts
    // point to the first profile and be badged/named appropriately.
    CreateOrUpdateShortcutsForProfileAtPath(GetOtherProfilePath(profile_path),
                                            UPDATE_EXISTING_ONLY,
                                            UPDATE_NON_PROFILE_SHORTCUTS);
  }
}

void ProfileShortcutManagerWin::OnProfileWasRemoved(
    const base::FilePath& profile_path,
    const base::string16& profile_name) {
  ProfileAttributesStorage& storage =
      profile_manager_->GetProfileAttributesStorage();
  // If there is only one profile remaining, remove the badging information
  // from an existing shortcut.
  const bool deleting_down_to_last_profile =
      (storage.GetNumberOfProfiles() == 1u);
  if (deleting_down_to_last_profile) {
    // This is needed to unbadge the icon.
    CreateOrUpdateShortcutsForProfileAtPath(
        storage.GetAllProfilesAttributes().front()->GetPath(),
        UPDATE_EXISTING_ONLY, IGNORE_NON_PROFILE_SHORTCUTS);
  }

  base::CreateCOMSTATaskRunnerWithTraits({base::MayBlock()})
      ->PostTask(FROM_HERE,
                 base::BindOnce(&DeleteDesktopShortcuts, profile_path,
                                deleting_down_to_last_profile));
}

void ProfileShortcutManagerWin::OnProfileNameChanged(
    const base::FilePath& profile_path,
    const base::string16& old_profile_name) {
  CreateOrUpdateShortcutsForProfileAtPath(profile_path, UPDATE_EXISTING_ONLY,
                                          IGNORE_NON_PROFILE_SHORTCUTS);
}

void ProfileShortcutManagerWin::OnProfileAvatarChanged(
    const base::FilePath& profile_path) {
  CreateOrUpdateProfileIcon(profile_path);
}

base::FilePath ProfileShortcutManagerWin::GetOtherProfilePath(
    const base::FilePath& profile_path) {
  const ProfileAttributesStorage& storage =
      profile_manager_->GetProfileAttributesStorage();
  DCHECK_EQ(2u, storage.GetNumberOfProfiles());
  // Get the index of the current profile, in order to find the index of the
  // other profile.
  std::vector<ProfileAttributesEntry*> entries =
      g_browser_process->profile_manager()
          ->GetProfileAttributesStorage()
          .GetAllProfilesAttributes();
  for (ProfileAttributesEntry* entry : entries) {
    base::FilePath path = entry->GetPath();
    if (path != profile_path)
      return path;
  }
  NOTREACHED();
  return base::FilePath();
}

void ProfileShortcutManagerWin::CreateOrUpdateShortcutsForProfileAtPath(
    const base::FilePath& profile_path,
    CreateOrUpdateMode create_mode,
    NonProfileShortcutAction action) {
  DCHECK(!BrowserThread::IsThreadInitialized(BrowserThread::UI) ||
         BrowserThread::CurrentlyOn(BrowserThread::UI));
  CreateOrUpdateShortcutsParams params(profile_path, create_mode, action);

  ProfileAttributesStorage& storage =
      profile_manager_->GetProfileAttributesStorage();
  ProfileAttributesEntry* entry;
  bool has_entry = storage.GetProfileAttributesWithPath(profile_path, &entry);

  if (!has_entry)
    return;
  bool remove_badging = (storage.GetNumberOfProfiles() == 1u);

  params.old_profile_name = entry->GetShortcutName();

  // Exit early if the mode is to update existing profile shortcuts only and
  // none were ever created for this profile, per the shortcut name not being
  // set in the profile attributes storage.
  if (params.old_profile_name.empty() && create_mode == UPDATE_EXISTING_ONLY &&
      action == IGNORE_NON_PROFILE_SHORTCUTS) {
    return;
  }

  if (!remove_badging) {
    params.profile_name = entry->GetName();

    // The profile might be using the Gaia avatar, which is not in the
    // resources array.
    bool has_gaia_image = false;
    if (entry->IsUsingGAIAPicture()) {
      const gfx::Image* image = entry->GetGAIAPicture();
      if (image) {
        params.avatar_image_1x = GetSkBitmapCopy(*image);
        // Gaia images are 256px, which makes them big enough to use in the
        // large icon case as well.
        DCHECK_GE(image->Width(), IconUtil::kLargeIconSize);
        params.avatar_image_2x = params.avatar_image_1x;
        has_gaia_image = true;
      }
    }

    // If the profile isn't using a Gaia image, or if the Gaia image did not
    // exist, revert to the previously used avatar icon.
    if (!has_gaia_image) {
      const size_t icon_index = entry->GetAvatarIconIndex();
      const int resource_id_1x =
          profiles::GetDefaultAvatarIconResourceIDAtIndex(icon_index);
      const int resource_id_2x = kProfileAvatarIconResources2x[icon_index];
      // Make a copy of the SkBitmaps to ensure that we can safely use the image
      // data on the thread we post to.
      params.avatar_image_1x = GetImageResourceSkBitmapCopy(resource_id_1x);
      params.avatar_image_2x = GetImageResourceSkBitmapCopy(resource_id_2x);
    }
  }
  base::CreateCOMSTATaskRunnerWithTraits({base::MayBlock()})
      ->PostTask(FROM_HERE,
                 base::BindOnce(
                     &CreateOrUpdateDesktopShortcutsAndIconForProfile, params));

  entry->SetShortcutName(params.profile_name);
}

void ProfileShortcutManagerWin::Observe(
    int type,
    const content::NotificationSource& source,
    const content::NotificationDetails& details) {
  DCHECK_EQ(chrome::NOTIFICATION_PROFILE_CREATED, type);

  Profile* profile =
      content::Source<Profile>(source).ptr()->GetOriginalProfile();
  if (profile->GetPrefs()->GetInteger(prefs::kProfileIconVersion) <
      kCurrentProfileIconVersion) {
    // Ensure the profile's icon file has been created.
    CreateOrUpdateProfileIcon(profile->GetPath());
  }
}
