#!/usr/bin/env bash
set -euo pipefail

source_path="${1:-}"
if (($# != 1)) || [[ -z "$source_path" || ! -r "$source_path" ]]; then
    echo "usage: $0 UI_FEATURES_CC" >&2
    exit 2
fi

if ! grep -q '^bool IsNativeGuestOzonePlatform()' "$source_path"; then
    echo "missing Chrome WebUI platform gate" >&2
    exit 1
fi

webui_helpers=(
    IsWebUIReloadButtonEnabled
    IsWebUIHomeButtonEnabled
    IsWebUIBatterySaverButtonEnabled
    IsWebUIAppMenuButtonEnabled
    IsWebUIBackForwardButtonEnabled
    IsWebUIPinnedToolbarActionsEnabled
    IsWebUIExtensionsContainerEnabled
    IsWebUISplitTabsButtonEnabled
    IsWebUIAvatarButtonEnabled
    IsWebUIMediaButtonEnabled
    IsWebUIPerformanceInterventionButtonEnabled
    IsWebUILocationBarEnabled
    IsWebUIToolbarEnabled
)

for helper in "${webui_helpers[@]}"; do
    body="$(sed -n "/^bool ${helper}()/,/^}/p" "$source_path")"
    if ! grep -q 'IsNativeGuestOzonePlatform()' <<<"$body"; then
        echo "missing platform gate in ${helper}()" >&2
        exit 1
    fi
done

echo "Chrome WebUI mode: OK"
