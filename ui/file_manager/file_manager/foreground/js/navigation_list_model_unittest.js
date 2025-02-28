// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

'use strict';

/**
 * Mock Recent fake entry.
 * @type {!FilesAppEntry}
 */
const recentFakeEntry =
    /** @type {!FilesAppEntry} */ ({toURL: () => 'fake-entry://recent'});

/**
 * Directory model.
 * @type {!DirectoryModel}
 */
let directoryModel;

/**
 * Drive file system.
 * @type {!MockFileSystem}
 */
let drive;

/**
 * Removable volume file system.
 * @type {!MockFileSystem}
 */
let hoge;

// Setup the test components.
function setUp() {
  // Mock LoadTimeData strings.
  window.loadTimeData.data = {
    MY_FILES_VOLUME_ENABLED: false,
    MY_FILES_ROOT_LABEL: 'My files',
    DOWNLOADS_DIRECTORY_LABEL: 'Downloads',
    DRIVE_DIRECTORY_LABEL: 'My Drive',
  };
  window.loadTimeData.getString = id => {
    return window.loadTimeData.data_[id] || id;
  };

  // Mock chrome APIs.
  new MockCommandLinePrivate();

  // Override VolumeInfo.prototype.resolveDisplayRoot to be sync.
  VolumeInfoImpl.prototype.resolveDisplayRoot = function(successCallback) {
    this.displayRoot_ = this.fileSystem_.root;
    successCallback(this.displayRoot_);
    return Promise.resolve(this.fileSystem_.root);
  };

  // Create mock components.
  directoryModel = createFakeDirectoryModel();
  drive = new MockFileSystem('drive');
  hoge = new MockFileSystem('removable:hoge');
}

/**
 * Tests model.
 */
function testModel() {
  const volumeManager = new MockVolumeManager();

  const shortcutListModel = new MockFolderShortcutDataModel(
      [new MockFileEntry(drive, '/root/shortcut')]);
  const recentItem = new NavigationModelFakeItem(
      'recent-label', NavigationModelItemType.RECENT, recentFakeEntry);

  const crostiniFakeItem = new NavigationModelFakeItem(
      'linux-files-label', NavigationModelItemType.CROSTINI,
      new FakeEntry(
          'linux-files-label', VolumeManagerCommon.RootType.CROSTINI));

  const model = new NavigationListModel(
      volumeManager, shortcutListModel, recentItem, directoryModel);
  model.linuxFilesItem = crostiniFakeItem;

  assertEquals(4, model.length);
  assertEquals(
      'fake-entry://recent', /** @type {!NavigationModelFakeItem} */
      (model.item(0)).entry.toURL());
  assertEquals(
      '/root/shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(1)).entry.fullPath);
  assertEquals('My files', model.item(2).label);
  assertEquals(
      'drive', /** @type {!NavigationModelVolumeItem} */
      (model.item(3)).volumeInfo.volumeId);

  // Downloads and Crostini are displayed within My files.
  const myFilesItem = /** @type NavigationModelFakeItem */ (model.item(2));
  const myFilesEntryList = /** @type {!EntryList} */ (myFilesItem.entry);
  assertEquals(2, myFilesEntryList.getUIChildren().length);
  assertEquals('Downloads', myFilesEntryList.getUIChildren()[0].name);
  assertEquals('linux-files-label', myFilesEntryList.getUIChildren()[1].name);
}

/**
 * Tests model with no Recents, Linux files, Play files.
 */
function testNoRecentOrLinuxFiles() {
  const volumeManager = new MockVolumeManager();

  const shortcutListModel = new MockFolderShortcutDataModel(
      [new MockFileEntry(drive, '/root/shortcut')]);
  const recentItem = null;

  const model = new NavigationListModel(
      volumeManager, shortcutListModel, recentItem, directoryModel);

  assertEquals(3, model.length);
  assertEquals(
      '/root/shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).entry.fullPath);
  assertEquals('My files', model.item(1).label);
  assertEquals(
      'drive', /** @type {!NavigationModelVolumeItem} */
      (model.item(2)).volumeInfo.volumeId);
}

/**
 * Tests adding and removing shortcuts.
 */
function testAddAndRemoveShortcuts() {
  const volumeManager = new MockVolumeManager();

  const shortcutListModel = new MockFolderShortcutDataModel(
      [new MockFileEntry(drive, '/root/shortcut')]);
  const recentItem = null;

  const model = new NavigationListModel(
      volumeManager, shortcutListModel, recentItem, directoryModel);

  assertEquals(3, model.length);

  // Add a shortcut at the tail, shortcuts are sorted by their label.
  const addShortcut = new MockFileEntry(drive, '/root/shortcut2');
  shortcutListModel.splice(1, 0, addShortcut);

  assertEquals(4, model.length);
  assertEquals(
      'shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).label);
  assertEquals(
      'shortcut2', /** @type {!NavigationModelShortcutItem} */
      (model.item(1)).label);

  // Add a shortcut at the head.
  const headShortcut = new MockFileEntry(drive, '/root/head');
  shortcutListModel.splice(0, 0, headShortcut);

  assertEquals(5, model.length);
  assertEquals(
      'head', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).label);
  assertEquals(
      'shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(1)).label);
  assertEquals(
      'shortcut2', /** @type {!NavigationModelShortcutItem} */
      (model.item(2)).label);

  // Remove the last shortcut.
  shortcutListModel.splice(2, 1);

  assertEquals(4, model.length);
  assertEquals(
      'head', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).label);
  assertEquals(
      'shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(1)).label);

  // Remove the first shortcut.
  shortcutListModel.splice(0, 1);

  assertEquals(3, model.length);
  assertEquals(
      'shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).label);
}

/**
 * Tests adding and removing volumes.
 */
function testAddAndRemoveVolumes() {
  const volumeManager = new MockVolumeManager();

  const shortcutListModel = new MockFolderShortcutDataModel(
      [new MockFileEntry(drive, '/root/shortcut')]);
  const recentItem = null;

  const model = new NavigationListModel(
      volumeManager, shortcutListModel, recentItem, directoryModel);

  assertEquals(3, model.length);

  // Mount removable volume 'hoge'.
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.REMOVABLE, 'removable:hoge', '',
      'device/path/1'));

  assertEquals(4, model.length);
  assertEquals(
      '/root/shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).entry.fullPath);
  assertEquals('My files', model.item(1).label);
  assertEquals(
      'drive', /** @type {!NavigationModelVolumeItem} */
      (model.item(2)).volumeInfo.volumeId);
  assertEquals(
      'removable:hoge', /** @type {!NavigationModelVolumeItem} */
      (model.item(3)).volumeInfo.volumeId);

  // Mount removable volume 'fuga'. Not a partition, so set a different device
  // path to 'hoge'.
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.REMOVABLE, 'removable:fuga', '',
      'device/path/2'));

  assertEquals(5, model.length);
  assertEquals(
      '/root/shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).entry.fullPath);
  assertEquals('My files', model.item(1).label);
  assertEquals(
      'drive', /** @type {!NavigationModelVolumeItem} */
      (model.item(2)).volumeInfo.volumeId);
  assertEquals(
      'removable:hoge', /** @type {!NavigationModelVolumeItem} */
      (model.item(3)).volumeInfo.volumeId);
  assertEquals(
      'removable:fuga', /** @type {!NavigationModelVolumeItem} */
      (model.item(4)).volumeInfo.volumeId);

  // Create a shortcut on the 'hoge' volume.
  shortcutListModel.splice(
      1, 0, new MockFileEntry(hoge, '/shortcut2'));

  assertEquals(6, model.length);
  assertEquals(
      '/root/shortcut', /** @type {!NavigationModelShortcutItem} */
      (model.item(0)).entry.fullPath);
  assertEquals(
      '/shortcut2', /** @type {!NavigationModelShortcutItem} */
      (model.item(1)).entry.fullPath);
  assertEquals('My files', model.item(2).label);
  assertEquals(
      'drive', /** @type {!NavigationModelVolumeItem} */
      (model.item(3)).volumeInfo.volumeId);
  assertEquals(
      'removable:hoge', /** @type {!NavigationModelVolumeItem} */
      (model.item(4)).volumeInfo.volumeId);
  assertEquals(
      'removable:fuga', /** @type {!NavigationModelVolumeItem} */
      (model.item(5)).volumeInfo.volumeId);
}

/**
 * Tests that orderAndNestItems_ function does the following:
 * 1. produces the expected order of volumes.
 * 2. manages NavigationSection for the relevant volumes.
 * 3. keeps MTP/Archive/Removable volumes on the original order.
 */
function testOrderAndNestItems() {
  const volumeManager = new MockVolumeManager();

  const shortcutListModel = new MockFolderShortcutDataModel([
    new MockFileEntry(drive, '/root/shortcut'),
    new MockFileEntry(drive, '/root/shortcut2')
  ]);

  const recentItem = new NavigationModelFakeItem(
      'recent-label', NavigationModelItemType.RECENT, recentFakeEntry);

  // Create different volumes.
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.PROVIDED, 'provided:prov1'));
  // Set the device paths of the removable volumes to different strings to
  // test the behaviour of two physically separate external devices.
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.REMOVABLE, 'removable:hoge', '',
      'device/path/1'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.REMOVABLE, 'removable:fuga', '',
      'device/path/2'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.ARCHIVE, 'archive:a-rar'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.MTP, 'mtp:a-phone'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.PROVIDED, 'provided:prov2'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.ANDROID_FILES, 'android_files:droid'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.MEDIA_VIEW, 'media_view:images_root'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.MEDIA_VIEW, 'media_view:videos_root'));
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.MEDIA_VIEW, 'media_view:audio_root'));

  // ZipArchiver mounts zip files as a PROVIDED volume type.
  const zipVolumeId = 'provided:dmboannefpncccogfdikhmhpmdnddgoe:' +
      '~%2FDownloads%2Fazip_file%2Ezip:' +
      '096eaa592ea7e8ffb9a27435e50dabd6c809c125';
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.PROVIDED, zipVolumeId));

  // Navigation items built above:
  //  1.  fake-entry://recent
  //  2.  media_view:images_root
  //  3.  media_view:videos_root
  //  4.  media_view:audio_root
  //  5.  /root/shortcut
  //  6.  /root/shortcut2
  //  7.  My files
  //        -> Downloads
  //        -> Play files
  //        -> Linux files
  //  8.  Drive  - from setup()
  //  9.  provided:prov1
  // 10.  provided:prov2
  //
  // 11.  removable:hoge
  // 12.  removable:fuga
  // 13.  archive:a-rar  - mounted as archive
  // 14.  mtp:a-phone
  // 15.  provided:"zip" - mounted as provided: $zipVolumeId

  // Constructor already calls orderAndNestItems_.
  const model = new NavigationListModel(
      volumeManager, shortcutListModel, recentItem, directoryModel);

  // Check items order and that MTP/Archive/Removable respect the original
  // order.
  assertEquals(15, model.length);
  assertEquals('recent-label', model.item(0).label);

  assertEquals('media_view:images_root', model.item(1).label);
  assertEquals('media_view:videos_root', model.item(2).label);
  assertEquals('media_view:audio_root', model.item(3).label);

  assertEquals('shortcut', model.item(4).label);
  assertEquals('shortcut2', model.item(5).label);
  assertEquals('My files', model.item(6).label);

  assertEquals('My Drive', model.item(7).label);
  assertEquals('provided:prov1', model.item(8).label);
  assertEquals('provided:prov2', model.item(9).label);

  assertEquals('removable:hoge', model.item(10).label);
  assertEquals('removable:fuga', model.item(11).label);

  assertEquals('archive:a-rar', model.item(12).label);
  assertEquals('mtp:a-phone', model.item(13).label);
  assertEquals(zipVolumeId, model.item(14).label);

  // Check NavigationSection, which defaults to TOP.
  // recent-label.
  assertEquals(NavigationSection.TOP, model.item(0).section);
  // Media View.
  // Images.
  assertEquals(NavigationSection.TOP, model.item(1).section);
  // Videos.
  assertEquals(NavigationSection.TOP, model.item(2).section);
  // Audio.
  assertEquals(NavigationSection.TOP, model.item(3).section);
  // shortcut.
  assertEquals(NavigationSection.TOP, model.item(4).section);
  // shortcut2.
  assertEquals(NavigationSection.TOP, model.item(5).section);

  // My Files.
  assertEquals(NavigationSection.MY_FILES, model.item(6).section);

  // Drive and FSP are grouped together.
  // My Drive.
  assertEquals(NavigationSection.CLOUD, model.item(7).section);
  // provided:prov1.
  assertEquals(NavigationSection.CLOUD, model.item(8).section);
  // provided:prov2.
  assertEquals(NavigationSection.CLOUD, model.item(9).section);

  // MTP/Archive/Removable are grouped together.
  // removable:hoge.
  assertEquals(NavigationSection.REMOVABLE, model.item(10).section);
  // removable:fuga.
  assertEquals(NavigationSection.REMOVABLE, model.item(11).section);
  // archive:a-rar.
  assertEquals(NavigationSection.REMOVABLE, model.item(12).section);
  // mtp:a-phone.
  assertEquals(NavigationSection.REMOVABLE, model.item(13).section);
  // archive:"zip" - $zipVolumeId
  assertEquals(NavigationSection.REMOVABLE, model.item(14).section);

  const myFilesModel = model.item(6);
  // Re-order again: cast to allow calling this private model function.
  /** @type {!Object} */ (model).orderAndNestItems_();
  // Check if My Files is still in the same position.
  assertEquals(NavigationSection.MY_FILES, model.item(6).section);
  // Check if My Files model is still the same instance, because DirectoryTree
  // expects it to be the same instance to be able to find it on the tree.
  assertEquals(myFilesModel, model.item(6));
}

/**
 * Tests model with My files enabled.
 */
function testMyFilesVolumeEnabled(callback) {
  // Enable My files.
  loadTimeData.data_['MY_FILES_VOLUME_ENABLED'] = true;

  const volumeManager = new MockVolumeManager();
  // Item 1 of the volume info list should have Downloads volume type.
  assertEquals(
      VolumeManagerCommon.VolumeType.DOWNLOADS,
      volumeManager.volumeInfoList.item(1).volumeType);
  // Create a downloads folder inside the item.
  const downloadsVolume = volumeManager.volumeInfoList.item(1);
  /** @type {!MockFileSystem} */ (downloadsVolume.fileSystem).populate([
    '/Downloads/'
  ]);

  const shortcutListModel = new MockFolderShortcutDataModel([]);
  const recentItem = null;

  // Create Android 'Play files' volume.
  volumeManager.volumeInfoList.add(MockVolumeManager.createMockVolumeInfo(
      VolumeManagerCommon.VolumeType.ANDROID_FILES, 'android_files:droid'));

  const crostiniFakeItem = new NavigationModelFakeItem(
      'linux-files-label', NavigationModelItemType.CROSTINI,
      new FakeEntry(
          'linux-files-label', VolumeManagerCommon.RootType.CROSTINI));

  // Navigation items built above:
  //  1. My files
  //       -> Play files
  //       -> Linux files
  //  2. Drive  - added by default by MockVolumeManager.

  // Constructor already calls orderAndNestItems_.
  const model = new NavigationListModel(
      volumeManager, shortcutListModel, recentItem, directoryModel);
  model.linuxFilesItem = crostiniFakeItem;

  assertEquals(2, model.length);
  assertEquals('My files', model.item(0).label);
  assertEquals('My Drive', model.item(1).label);

  // Android and Crostini are displayed within My files. And there is no
  // Downloads volume inside it. Downloads should be a normal folder inside
  // the My files volume.
  const myFilesItem = /** @type NavigationModelFakeItem */ (model.item(0));
  const myFilesEntryList = /** @type {!EntryList} */ (myFilesItem.entry);
  assertEquals(2, myFilesEntryList.getUIChildren().length);
  assertEquals('android_files:droid', myFilesEntryList.getUIChildren()[0].name);
  assertEquals('linux-files-label', myFilesEntryList.getUIChildren()[1].name);

  const reader = myFilesEntryList.createReader();
  const foundEntries = [];
  reader.readEntries((entries) => {
    for (const entry of entries) {
      foundEntries.push(entry);
    }
  });

  reportPromise(
      waitUntil(() => {
        // Wait for Downloads folder to be read from My files volume.
        return foundEntries.length >= 1;
      }).then(() => {
        assertEquals(foundEntries[0].name, 'Downloads');
        assertTrue(foundEntries[0].isDirectory);
      }),
      callback);
}
