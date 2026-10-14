# Match a complete record token; an '=' suffix intentionally begins a value.
function Test-LogRecord([string]$Text, [string]$Marker) {
    if ([string]::IsNullOrEmpty($Marker)) { return $false }
    $suffix = if ($Marker.EndsWith('=')) { '' } else { '(?=[ \t\r\n]|$)' }
    return $Text -match ('(?m)(?:^|[ \t])' + [regex]::Escape($Marker) + $suffix)
}
