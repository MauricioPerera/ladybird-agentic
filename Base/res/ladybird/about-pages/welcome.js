/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const form = document.querySelector("#welcome-form");
const setupOptions = document.querySelector("#setup-options");
const startBrowsing = document.querySelector("#start-browsing");
const searchEngines = document.querySelector("#search-engines");
const searchSuggestions = document.querySelector("#search-suggestions");
const searchSuggestionsEnabled = document.querySelector("#search-suggestions-enabled");
const searchSuggestionsDescription = document.querySelector("#search-suggestions-description");
const tabLayoutInputs = document.querySelectorAll('input[name="tab-layout"]');
const colorThemeInputs = document.querySelectorAll('input[name="color-theme"]');
const enableContentBlocking = document.querySelector("#enable-content-blocking");
const enableUpdateChecks = document.querySelector("#enable-update-checks");
const defaultBrowser = document.querySelector("#default-browser");
const defaultBrowserLabel = document.querySelector("#default-browser-label");
const defaultBrowserDescription = document.querySelector("#default-browser-description");
const makeDefaultBrowser = document.querySelector("#make-default-browser");
const saveStatus = document.querySelector("#save-status");

const containersList = document.querySelector("#containers-list");
const customContainerName = document.querySelector("#custom-container-name");
const customContainerColor = document.querySelector("#custom-container-color");
const createContainerBtn = document.querySelector("#create-container-btn");
const containerStatus = document.querySelector("#container-status");

const mcpStatusPill = document.querySelector("#mcp-status-pill");
const enableMcpNow = document.querySelector("#enable-mcp-now");
const enableMcpAutoStart = document.querySelector("#enable-mcp-auto-start");
const mcpConfigBox = document.querySelector("#mcp-config-box");
const mcpConfigPreview = document.querySelector("#mcp-config-preview");
const copyMcpConfig = document.querySelector("#copy-mcp-config");
const mcpActionStatus = document.querySelector("#mcp-action-status");
const uploadDirInput = document.querySelector("#upload-dir-input");
const uploadDirSave = document.querySelector("#upload-dir-save");
const uploadDirReset = document.querySelector("#upload-dir-reset");
const uploadDirOpen = document.querySelector("#upload-dir-open");
const uploadDirStatus = document.querySelector("#upload-dir-status");
const uaPresetSelect = document.querySelector("#ua-preset-select");

const recMicrophone = document.querySelector("#rec-microphone");
const recSystemAudio = document.querySelector("#rec-system-audio");
const recShowCursor = document.querySelector("#rec-show-cursor");
const recSpeakerTimeline = document.querySelector("#rec-speaker-timeline");
const recFps = document.querySelector("#rec-fps");
const recBatchDuration = document.querySelector("#rec-batch-duration");

const BUILTIN_CONTAINERS = new Set(["personal", "work", "banking", "shopping", "agent"]);

let settings;
let engines;
let features;
let initialized = false;
// Why the last attempt to make Ladybird the default browser failed, which is shown in place of the row's description.
let defaultBrowserFailure = null;
let uaPresetsRendered = false;

function showInlineStatus(element, text) {
    element.textContent = text || "";
    element.classList.toggle("hidden", !text);
}

function setSearchEngineSettings(name, suggestions) {
    // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
    settings.searchEngine = { ...settings.searchEngine, engine: name, suggestions };
    ladybird.sendMessage("setSearchEngineSettings", { engine: name, suggestions });
    updateSearchSuggestions();
}

function setSearchEngine(name) {
    setSearchEngineSettings(name, name !== null || settings.searchEngine.suggestions);
}

// The suggestions switch is only shown for a chosen engine that offers suggestions, and says where typed text goes.
function updateSearchSuggestions() {
    const name = settings.searchEngine.engine;
    const engine = engines.search.find(engine => engine.name === name);
    searchSuggestions.classList.toggle("hidden", !engine?.supportsSuggestions);
    searchSuggestionsEnabled.checked = settings.searchEngine.suggestions;
    if (engine)
        searchSuggestionsDescription.textContent = `Your typing in the address bar is sent to ${name} before you press Enter.`;
}

function renderSearchEngines() {
    const names = engines.search.map(engine => engine.name).sort((a, b) => a.localeCompare(b));

    searchEngines.replaceChildren();
    for (const name of names) {
        const tile = document.createElement("label");
        tile.className = "search-engine choice-card";

        const radio = document.createElement("input");
        radio.type = "radio";
        radio.name = "search-engine";
        radio.value = name;
        radio.checked = settings.searchEngine.engine === name;
        radio.addEventListener("change", () => {
            if (!initialized) return;
            setSearchEngine(name);
        });

        // A search engine is optional, so choosing the chosen one again goes without one.
        let wasChecked = false;
        tile.addEventListener("pointerdown", () => {
            wasChecked = radio.checked;
        });
        tile.addEventListener("click", event => {
            if (!initialized || !wasChecked || event.target !== radio) return;
            wasChecked = false;
            radio.checked = false;
            setSearchEngine(null);
        });

        // NB: Engines are shown by name only. Logos would mean shipping other companies' trademarks, or contacting
        //     their servers before anyone has chosen.
        const label = document.createElement("span");
        label.textContent = name;

        tile.append(radio, label);
        searchEngines.append(tile);
    }
}

function tabLayout() {
    if (!settings.tabs.verticalTabsEnabled) return "horizontal";
    return settings.tabs.everythingInSidebar ? "sidebar" : "vertical";
}

function updateTabLayout() {
    const layout = tabLayout();
    for (const input of tabLayoutInputs) input.checked = input.value === layout;
}

function updateColorTheme() {
    const colorTheme = settings.appearance.colorTheme || "neutral";
    for (const input of colorThemeInputs) input.checked = input.value === colorTheme;
}

// The row is only shown where the system can tell which browser is the default, and lets Ladybird become it.
function updateDefaultBrowser() {
    const isDefault = settings.defaultBrowser === "default";
    const canMakeDefault = settings.defaultBrowser === "not-default";

    defaultBrowser.classList.toggle("hidden", !isDefault && !canMakeDefault);
    defaultBrowserDescription.textContent =
        defaultBrowserFailure ??
        (isDefault ? "Links from other apps open in Ladybird." : "Open links from other apps in Ladybird.");
    makeDefaultBrowser.classList.toggle("hidden", !canMakeDefault);

    // A control row is filled under the pointer, and a click anywhere on it reaches the control its label names. This
    // row is one only while there is something for the button to do.
    defaultBrowser.classList.toggle("control-row", canMakeDefault);
    if (canMakeDefault) defaultBrowserLabel.htmlFor = makeDefaultBrowser.id;
    else defaultBrowserLabel.removeAttribute("for");
}

function updateUpdateChecks() {
    const networkFeatures = settings.backgroundNetworking.features;
    if (Object.hasOwn(networkFeatures, "updateChecks")) enableUpdateChecks.checked = networkFeatures.updateChecks;
    else enableUpdateChecks.closest(".switch-row").remove();
}

function updateContainers() {
    const containers = Array.isArray(settings.containers) ? settings.containers : [];
    containersList.replaceChildren();
    for (const container of containers) {
        const chip = document.createElement("span");
        chip.className = "container-chip";

        const dot = document.createElement("span");
        dot.className = "container-dot";
        dot.style.backgroundColor = container.color || "#3b82f6";

        const label = document.createElement("span");
        label.textContent = `${container.name} (${container.id})`;

        chip.append(dot, label);
        if (!BUILTIN_CONTAINERS.has(container.id)) {
            const removeBtn = document.createElement("button");
            removeBtn.type = "button";
            removeBtn.className = "container-delete";
            removeBtn.title = `Remove ${container.name}`;
            removeBtn.textContent = "×";
            removeBtn.addEventListener("click", () => {
                if (!initialized) return;
                showInlineStatus(containerStatus, "");
                ladybird.sendMessage("deleteContainer", container.id);
            });
            chip.append(removeBtn);
        }
        containersList.append(chip);
    }
}

function updateMcpSection() {
    const mcp = settings.mcp || {};
    const enabled = Boolean(mcp.enabled);
    const port = mcp.port || 7331;
    enableMcpNow.checked = enabled;
    enableMcpAutoStart.checked = Boolean(mcp.autoStart);
    mcpStatusPill.textContent = enabled ? `Listening on 127.0.0.1:${port}` : "Off";
    mcpStatusPill.classList.toggle("pill-on", enabled);

    const hasConfig = enabled && typeof mcp.configJson === "string" && mcp.configJson.length > 0;
    mcpConfigBox.classList.toggle("hidden", !hasConfig);
    mcpConfigPreview.textContent = hasConfig ? mcp.configJson : "";
}

function updateUploadDirectory() {
    const overridden = Boolean(settings.uploadDirectoryOverriddenByCli);
    if (document.activeElement !== uploadDirInput) {
        uploadDirInput.value = settings.effectiveUploadDirectory || settings.defaultUploadDirectory || "";
    }
    uploadDirInput.disabled = overridden;
    uploadDirSave.disabled = overridden;
    uploadDirReset.disabled = overridden;
    if (overridden)
        showInlineStatus(uploadDirStatus, "Managed by --mcp-upload-dir for this session.");
}

function updateUserAgentPreset() {
    if (!uaPresetsRendered) {
        uaPresetsRendered = true;
        const presets = Array.isArray(settings.availableUserAgentPresets) ? settings.availableUserAgentPresets : [];
        for (const name of presets) {
            const opt = document.createElement("option");
            opt.value = name;
            opt.textContent = name;
            uaPresetSelect.append(opt);
        }
    }
    uaPresetSelect.value = settings.userAgentPreset || "";
}

function updateTabRecorder() {
    const rec = settings.tabRecorder || {};
    recMicrophone.checked = rec.recordMicrophone !== false;
    recSystemAudio.checked = rec.recordSystemAudio !== false;
    recShowCursor.checked = rec.showCursor !== false;
    recSpeakerTimeline.checked = rec.exportSpeakerTimeline !== false;
    recFps.value = String(rec.fps || 30);
    recBatchDuration.value = String(rec.batchDurationSeconds || 300);
}

function initialize() {
    if (initialized || !settings || !engines || !features) return;
    initialized = true;

    document.querySelector("#tab-layout-section").classList.toggle("hidden", !features.verticalTabs);
    updateTabLayout();
    updateColorTheme();
    updateDefaultBrowser();

    renderSearchEngines();
    updateSearchSuggestions();

    enableContentBlocking.checked = settings.contentBlockers.enabled;
    updateUpdateChecks();
    updateContainers();
    updateMcpSection();
    updateUploadDirectory();
    updateUserAgentPreset();
    updateTabRecorder();

    setupOptions.disabled = false;
    startBrowsing.disabled = false;
}

for (const input of tabLayoutInputs) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked || !features.verticalTabs) return;
        // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
        settings.tabs = {
            ...settings.tabs,
            verticalTabsEnabled: input.value !== "horizontal",
            everythingInSidebar: input.value === "sidebar",
        };
        ladybird.sendMessage("setTabSettings", settings.tabs);
    });
}

// Picking a color recolors the window around the page at once.
for (const input of colorThemeInputs) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked) return;
        // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
        settings.appearance = { ...settings.appearance, colorTheme: input.value };
        ladybird.sendMessage("setAppearance", settings.appearance);
    });
}

searchSuggestionsEnabled.addEventListener("change", () => {
    if (!initialized) return;
    setSearchEngineSettings(settings.searchEngine.engine, searchSuggestionsEnabled.checked);
});

enableContentBlocking.addEventListener("change", () => {
    if (!initialized) return;

    // Enable the lists before the content blocker, which then downloads every enabled list.
    if (enableContentBlocking.checked) {
        for (const identifier of ["easyList", "easyPrivacy"])
            ladybird.sendMessage("setContentBlockerListEnabled", { identifier, enabled: true });
    }

    ladybird.sendMessage("setContentBlockerEnabled", enableContentBlocking.checked);
    ladybird.sendMessage("setFilterListUpdatesEnabled", enableContentBlocking.checked);
});

enableUpdateChecks.addEventListener("change", () => {
    if (!initialized) return;
    ladybird.sendMessage("setUpdateChecksEnabled", enableUpdateChecks.checked);
});

// The system may ask before it changes the default browser, so the button waits for its answer.
makeDefaultBrowser.addEventListener("click", () => {
    if (!initialized) return;

    makeDefaultBrowser.disabled = true;
    defaultBrowserFailure = null;
    updateDefaultBrowser();

    ladybird.sendMessage("makeDefaultBrowser");
});

createContainerBtn.addEventListener("click", () => {
    if (!initialized) return;
    const name = customContainerName.value.trim();
    if (!name) {
        showInlineStatus(containerStatus, "Enter a container name first.");
        return;
    }
    showInlineStatus(containerStatus, "");
    createContainerBtn.disabled = true;
    ladybird.sendMessage("createContainer", {
        name,
        color: customContainerColor.value,
    });
});

customContainerName.addEventListener("keydown", event => {
    if (event.key === "Enter") {
        event.preventDefault();
        createContainerBtn.click();
    }
});

enableMcpNow.addEventListener("change", () => {
    if (!initialized) return;
    showInlineStatus(mcpActionStatus, "");
    ladybird.sendMessage("setMcpEnabled", enableMcpNow.checked);
});

enableMcpAutoStart.addEventListener("change", () => {
    if (!initialized) return;
    ladybird.sendMessage("setMcpAutoStart", enableMcpAutoStart.checked);
});

copyMcpConfig.addEventListener("click", async () => {
    const text = mcpConfigPreview.textContent;
    if (!text) return;
    let copied = false;
    try {
        if (navigator.clipboard?.writeText) {
            await navigator.clipboard.writeText(text);
            copied = true;
        }
    } catch {
        copied = false;
    }
    if (!copied) {
        const tmp = document.createElement("textarea");
        tmp.value = text;
        tmp.style.position = "fixed";
        tmp.style.opacity = "0";
        document.body.append(tmp);
        tmp.select();
        copied = document.execCommand("copy");
        tmp.remove();
    }
    copyMcpConfig.textContent = copied ? "Copied!" : "Copy failed";
    setTimeout(() => {
        copyMcpConfig.textContent = "Copy Config";
    }, 1500);
});

uploadDirSave.addEventListener("click", () => {
    if (!initialized) return;
    showInlineStatus(uploadDirStatus, "");
    ladybird.sendMessage("setUploadDirectory", uploadDirInput.value);
});

uploadDirReset.addEventListener("click", () => {
    if (!initialized) return;
    showInlineStatus(uploadDirStatus, "");
    uploadDirInput.value = "";
    ladybird.sendMessage("setUploadDirectory", "");
});

uploadDirOpen.addEventListener("click", () => {
    if (!initialized) return;
    ladybird.sendMessage("openUploadDirectory");
});

uaPresetSelect.addEventListener("change", () => {
    if (!initialized) return;
    ladybird.sendMessage("setUserAgentPreset", uaPresetSelect.value);
});

function sendTabRecorderPatch(patch) {
    if (!initialized) return;
    settings.tabRecorder = { ...settings.tabRecorder, ...patch };
    ladybird.sendMessage("setTabRecorderSettings", patch);
}

recMicrophone.addEventListener("change", () => sendTabRecorderPatch({ recordMicrophone: recMicrophone.checked }));
recSystemAudio.addEventListener("change", () => sendTabRecorderPatch({ recordSystemAudio: recSystemAudio.checked }));
recShowCursor.addEventListener("change", () => sendTabRecorderPatch({ showCursor: recShowCursor.checked }));
recSpeakerTimeline.addEventListener("change", () =>
    sendTabRecorderPatch({ exportSpeakerTimeline: recSpeakerTimeline.checked })
);
recFps.addEventListener("change", () => sendTabRecorderPatch({ fps: Number(recFps.value) }));
recBatchDuration.addEventListener("change", () =>
    sendTabRecorderPatch({ batchDurationSeconds: Number(recBatchDuration.value) })
);

form.addEventListener("submit", event => {
    event.preventDefault();
    if (!initialized || startBrowsing.disabled) return;

    setupOptions.disabled = true;
    startBrowsing.disabled = true;
    saveStatus.textContent = "";
    ladybird.sendMessage("completeFirstRun");
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadFeatures");
    ladybird.sendMessage("loadAvailableEngines");
    ladybird.sendMessage("loadCurrentSettings");
});

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;

    if (name === "loadSettings") {
        settings = { ...settings, ...data };

        if (initialized) {
            if (Object.hasOwn(data, "tabs")) updateTabLayout();
            if (Object.hasOwn(data, "appearance")) updateColorTheme();
            if (Object.hasOwn(data, "searchEngine")) {
                for (const radio of searchEngines.querySelectorAll("input"))
                    radio.checked = radio.value === settings.searchEngine.engine;
                updateSearchSuggestions();
            }
            if (Object.hasOwn(data, "contentBlockers"))
                enableContentBlocking.checked = settings.contentBlockers.enabled;
            if (Object.hasOwn(data, "backgroundNetworking")) updateUpdateChecks();
            if (Object.hasOwn(data, "defaultBrowser")) {
                defaultBrowserFailure = null;
                updateDefaultBrowser();
            }
            if (Object.hasOwn(data, "containers")) updateContainers();
            if (Object.hasOwn(data, "mcp")) updateMcpSection();
            if (
                Object.hasOwn(data, "uploadDirectory") ||
                Object.hasOwn(data, "effectiveUploadDirectory") ||
                Object.hasOwn(data, "uploadDirectoryOverriddenByCli")
            ) {
                updateUploadDirectory();
            }
            if (Object.hasOwn(data, "userAgentPreset")) updateUserAgentPreset();
            if (Object.hasOwn(data, "tabRecorder")) updateTabRecorder();
        }
    } else if (name === "loadEngines") {
        engines = data;
    } else if (name === "loadFeatures") {
        features = data;
    } else if (name === "defaultBrowserResult") {
        makeDefaultBrowser.disabled = false;
        defaultBrowserFailure = data;
        updateDefaultBrowser();
    } else if (name === "containerActionResult") {
        createContainerBtn.disabled = false;
        if (data?.ok) {
            if (data.container) customContainerName.value = "";
            showInlineStatus(containerStatus, "");
        } else {
            showInlineStatus(containerStatus, data?.error || "Container update failed.");
        }
    } else if (name === "mcpActionResult") {
        showInlineStatus(mcpActionStatus, data?.ok ? "" : data?.error || "MCP action failed.");
    } else if (name === "uploadDirectoryStatus") {
        showInlineStatus(uploadDirStatus, data || "");
    } else if (name === "firstRunCompleted") {
        location.replace(data);
    } else if (name === "firstRunError") {
        saveStatus.textContent = data;
        setupOptions.disabled = false;
        startBrowsing.disabled = false;
    }

    initialize();
});
