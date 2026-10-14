# Normalize object-list data only; do not rewrite blank lines or comments.
function Test-ObjectListData([string]$Path) {
    return (-not [string]::IsNullOrWhiteSpace($Path) -and
            -not $Path.TrimStart().StartsWith('#'))
}

function ConvertTo-ObjectListPath([string]$Path) {
    if (-not (Test-ObjectListData $Path)) {
        return $Path
    }
    return $Path.Replace('\', '/')
}
