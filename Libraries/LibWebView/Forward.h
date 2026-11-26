/*
 * Copyright (c) 2022, The SerenityOS developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/Traits.h>
#include <LibWebCommon/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

class Application;
class ApplyHistoryStep;
class AudioServerControlClient;
class Autocomplete;
class AutocompleteService;
class BlobURLStore;
class BrowsingSession;
class CanonicalBrowsingContext;
class CanonicalBrowsingContextGroup;
class CanonicalDocument;
class CanonicalDocumentState;
class CanonicalEnvironmentSettingsObject;
class CanonicalWindowEnvironmentSettingsObject;
class CanonicalWorkerEnvironmentSettingsObject;
class CanonicalSessionHistoryEntry;
class CanonicalNavigable;
class CanonicalSimilarOriginWindowAgent;
class CanonicalTraversable;
class CanonicalWindow;
class CompositorClient;
class CompositorPresentationClient;
class ContainerStore;
class ContainerStoreObserver;
class CookieJar;
class DefaultBrowser;
class DownloadStore;
class ExtensionManager;
class ExtensionStore;
class ExtensionsUI;
class ExternalURLHandler;
class FaviconStore;
class FlowRunner;
class FlowStore;
class FlowsUI;
class FontService;
class FontServiceHost;
class HistoryStore;
class HSTSStore;
class ImageDecoderControlClient;
class ImageDecoderManager;
class MicrophoneAuthorizer;
class NavigationLoader;
class OutOfProcessWebView;
class NotificationBackend;
class NotificationManager;
class PageTranslation;
class PictureInPictureManager;
class RecordingNotificationBackend;
class PictureInPictureWindow;
class ProcessManager;
class RequestServerInstance;
class RequestServerManager;
class RequestServerSiteBindings;
class PermissionManager;
class Settings;
class CertificateUI;
class SettingsUI;
class SpellChecker;
class Translator;
class StorageJar;
class TabDiscarder;
class SystemProbeClient;
class TraversableSessionHistory;
class UpdateChecker;
class VideoDecoderServerControlClient;
class ViewImplementation;
class WebContentClient;
class WebContentTestClient;
class WebDriverBrowserConnection;
class WebWorkerClient;
class WebUI;

struct DownloadRecord;
struct ExtensionManifest;
struct BrowserOptions;
struct ChangingNavigable;
struct CookieStorageKey;
struct FlowDefinition;
struct FlowRunRecord;
struct HistoryEntry;
struct HistoryOperation;
struct InstalledExtension;
struct SearchEngine;
struct TabContainer;
struct UpdateCheckResult;
struct WebContentOptions;
class WebContentPage;

enum class UpdateCheckTrigger : u8;

}

namespace AK {

template<>
struct Traits<WebView::CookieStorageKey>;

}
