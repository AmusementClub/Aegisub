$ErrorActionPreference = 'Stop'

if (Test-Path -LiteralPath 'data') {
    icacls data /grant:r "${env:UserName}:F" /T | Out-Null
    Remove-Item -Path 'data' -Recurse -Force
}
New-Item -Path 'data' -ItemType 'directory' | Out-Null

New-Item -Path 'data\file' -ItemType 'file' | Out-Null
New-Item -Path 'data\dir' -ItemType 'directory' | Out-Null

New-Item -Path 'data\file_access_denied' -ItemType 'file' | Out-Null
icacls data\file_access_denied /deny "${env:UserName}:F" | Out-Null

New-Item -Path 'data\file_read_only' -ItemType 'file' | Out-Null
icacls data\file_read_only /deny "${env:UserName}:W" | Out-Null

New-Item -Path 'data\dir_access_denied' -ItemType 'directory' | Out-Null
icacls data\dir_access_denied /deny "${env:UserName}:F" | Out-Null

New-Item -Path 'data\dir_read_only' -ItemType 'directory' | Out-Null
icacls data\dir_read_only /deny "${env:UserName}:W" | Out-Null

New-Item -Path 'data\mru_ok.json' -ItemType 'file' -Value '{"Video" : ["Entry One", "Entry Two"]}' | Out-Null
New-Item -Path 'data\mru_invalid.json' -ItemType 'file' -Value '{"Video" : [1, 3]}' | Out-Null

New-Item -Path 'data\ten_bytes' -ItemType 'file' -Value '1234567890' | Out-Null
New-Item -Path 'data\touch_mod_time' -ItemType 'file' | Out-Null
(Get-ChildItem -Path 'data\touch_mod_time').LastWriteTime = (Get-ChildItem -Path 'data\touch_mod_time').LastWriteTime.AddSeconds(-1)

New-Item -Path 'data\dir_iterator' -ItemType 'directory' | Out-Null
New-Item -Path 'data\dir_iterator\1.a' -ItemType 'file' | Out-Null
New-Item -Path 'data\dir_iterator\2.a' -ItemType 'file' | Out-Null
New-Item -Path 'data\dir_iterator\1.b' -ItemType 'file' | Out-Null
New-Item -Path 'data\dir_iterator\2.b' -ItemType 'file' | Out-Null

Copy-Item -Path "${PSScriptRoot}\fixtures\options" -Destination 'data\options' -Recurse

New-Item -Path 'data\vfr' -ItemType 'directory' | Out-Null
Copy-Item -Path "${PSScriptRoot}\fixtures\vfr" -Destination 'data\vfr\in' -Recurse
New-Item -Path 'data\vfr\out' -ItemType 'directory' | Out-Null

Copy-Item -Path "${PSScriptRoot}\fixtures\keyframe" -Destination 'data\keyframe' -Recurse
