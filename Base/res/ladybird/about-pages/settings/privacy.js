import { getByteFormatter } from "../../utils.js";
import { registerDialogDeepLink } from "./dialog-deep-link.js";

const byteFormatter = getByteFormatter(unit => {
    return {
        unitDisplay: unit === "byte" ? "long" : "short",
        maximumFractionDigits: 1,
    };
});

const browsingDataSettings = document.querySelector("#browsing-data-settings");
const browsingDataSettingsClose = document.querySelector("#browsing-data-settings-close");
const browsingDataSettingsDialog = document.querySelector("#browsing-data-settings-dialog");
const browsingDataTotalSize = document.querySelector("#browsing-data-total-size");

const clearBrowsingDataCachedFiles = document.querySelector("#clear-browsing-data-cached-files");
const clearBrowsingDataCachedFilesSize = document.querySelector("#clear-browsing-data-cached-files-size");
const clearBrowsingDataDownloadHistory = document.querySelector("#clear-browsing-data-download-history");
const clearBrowsingDataHistory = document.querySelector("#clear-browsing-data-history");
const clearBrowsingDataRemoveData = document.querySelector("#clear-browsing-data-remove-data");
const clearBrowsingDataSiteData = document.querySelector("#clear-browsing-data-site-data");
const clearBrowsingDataSiteDataSize = document.querySelector("#clear-browsing-data-site-data-size");
const clearBrowsingDataTimeRange = document.querySelector("#clear-browsing-data-time-range");

const globalPrivacyControlToggle = document.querySelector("#global-privacy-control-toggle");
const userAgentPresetSelect = document.querySelector("#user-agent-preset");
const uploadDirectoryInput = document.querySelector("#upload-directory-input");
const uploadDirectorySave = document.querySelector("#upload-directory-save");
const uploadDirectoryReset = document.querySelector("#upload-directory-reset");
const uploadDirectoryOpen = document.querySelector("#upload-directory-open");
const uploadDirectoryStatus = document.querySelector("#upload-directory-status");

function loadSettings(settings) {
    if (Object.hasOwn(settings, "globalPrivacyControl")) {
        globalPrivacyControlToggle.checked = settings.globalPrivacyControl;
    }
    if (userAgentPresetSelect) {
        if (Array.isArray(settings.availableUserAgentPresets)) {
            userAgentPresetSelect.innerHTML = "";
            const defaultOption = document.createElement("option");
            defaultOption.value = "";
            defaultOption.textContent = "Default (Ladybird)";
            userAgentPresetSelect.appendChild(defaultOption);
            for (const preset of settings.availableUserAgentPresets) {
                const opt = document.createElement("option");
                opt.value = preset;
                opt.textContent = preset;
                userAgentPresetSelect.appendChild(opt);
            }
        }
        if (Object.hasOwn(settings, "userAgentPreset") || Array.isArray(settings.availableUserAgentPresets)) {
            userAgentPresetSelect.value = settings.userAgentPreset || "";
        }
    }
    if (uploadDirectoryInput && (Object.hasOwn(settings, "effectiveUploadDirectory") || Object.hasOwn(settings, "uploadDirectory") || Object.hasOwn(settings, "uploadDirectoryOverriddenByCli"))) {
        if (document.activeElement !== uploadDirectoryInput && (Object.hasOwn(settings, "effectiveUploadDirectory") || Object.hasOwn(settings, "uploadDirectory") || Object.hasOwn(settings, "defaultUploadDirectory"))) {
            uploadDirectoryInput.value = settings.effectiveUploadDirectory || settings.uploadDirectory || settings.defaultUploadDirectory || "";
        }
        if (Object.hasOwn(settings, "uploadDirectoryOverriddenByCli")) {
            const overridden = Boolean(settings.uploadDirectoryOverriddenByCli);
            uploadDirectoryInput.disabled = overridden;
            if (uploadDirectorySave) uploadDirectorySave.disabled = overridden;
            if (uploadDirectoryReset) uploadDirectoryReset.disabled = overridden;
            if (uploadDirectoryStatus) {
                uploadDirectoryStatus.innerText = overridden ? "Overridden by --mcp-upload-dir CLI flag." : "";
            }
        }
    }
}

function updateBrowsingDataSizes(sizes) {
    const totalSize = sizes.totalCacheSize + sizes.totalSiteDataSize;

    browsingDataTotalSize.innerText = `Your browsing data is currently using ${byteFormatter.formatBytes(totalSize)} of disk space`;

    clearBrowsingDataCachedFilesSize.innerText = ` (remove ${byteFormatter.formatBytes(sizes.cacheSizeSinceRequestedTime)})`;
    clearBrowsingDataSiteDataSize.innerText = ` (remove ${byteFormatter.formatBytes(sizes.siteDataSizeSinceRequestedTime)})`;
}

function computeTimeRange() {
    const now = Temporal.Now.zonedDateTimeISO();

    switch (clearBrowsingDataTimeRange.value) {
        case "lastHour":
            return now.subtract({ hours: 1 });
        case "last4Hours":
            return now.subtract({ hours: 4 });
        case "today":
            return now.startOfDay();
        case "all":
            return null;
        default:
            console.error(`Unrecognized time range: ${clearBrowsingDataTimeRange.value}`);
            return now;
    }
}

function estimateBrowsingDataSizes() {
    const since = computeTimeRange();

    ladybird.sendMessage("estimateBrowsingDataSizes", {
        since: since?.epochMilliseconds,
    });
}

registerDialogDeepLink({
    hash: "clearBrowsingData",
    tab: "privacy",
    dialog: browsingDataSettingsDialog,
    onOpen: estimateBrowsingDataSizes,
});

browsingDataSettings.addEventListener("click", () => {
    location.hash = "clearBrowsingData";
});

browsingDataSettingsClose.addEventListener("click", () => {
    browsingDataSettingsDialog.close();
});

clearBrowsingDataTimeRange.addEventListener("change", () => {
    estimateBrowsingDataSizes();
});

function setRemoveDataEnabledState() {
    clearBrowsingDataRemoveData.disabled =
        !clearBrowsingDataCachedFiles.checked &&
        !clearBrowsingDataHistory.checked &&
        !clearBrowsingDataDownloadHistory.checked &&
        !clearBrowsingDataSiteData.checked;
}

clearBrowsingDataCachedFiles.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataHistory.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataDownloadHistory.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataSiteData.addEventListener("change", setRemoveDataEnabledState);

clearBrowsingDataRemoveData.addEventListener("click", () => {
    const since = computeTimeRange();

    ladybird.sendMessage("clearBrowsingData", {
        since: since?.epochMilliseconds,
        cachedFiles: clearBrowsingDataCachedFiles.checked,
        history: clearBrowsingDataHistory.checked,
        downloadHistory: clearBrowsingDataDownloadHistory.checked,
        siteData: clearBrowsingDataSiteData.checked,
    });

    browsingDataSettingsDialog.close();
});

globalPrivacyControlToggle.addEventListener("change", () => {
    ladybird.sendMessage("setGlobalPrivacyControl", globalPrivacyControlToggle.checked);
});

if (userAgentPresetSelect) {
    userAgentPresetSelect.addEventListener("change", () => {
        ladybird.sendMessage("setUserAgentPreset", userAgentPresetSelect.value);
    });
}

if (uploadDirectorySave && uploadDirectoryInput) {
    uploadDirectorySave.addEventListener("click", () => {
        ladybird.sendMessage("setUploadDirectory", uploadDirectoryInput.value.trim());
    });
}

if (uploadDirectoryReset) {
    uploadDirectoryReset.addEventListener("click", () => {
        ladybird.sendMessage("setUploadDirectory", "");
    });
}

if (uploadDirectoryOpen) {
    uploadDirectoryOpen.addEventListener("click", () => {
        ladybird.sendMessage("openUploadDirectory");
    });
}

document.addEventListener("WebUILoaded", estimateBrowsingDataSizes);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "estimatedBrowsingDataSizes") {
        updateBrowsingDataSizes(event.detail.data);
    } else if (event.detail.name === "clearedBrowsingData") {
        estimateBrowsingDataSizes();
    } else if (event.detail.name === "uploadDirectoryStatus" && uploadDirectoryStatus) {
        uploadDirectoryStatus.innerText = event.detail.data || "";
    }
});
