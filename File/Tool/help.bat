@echo off
chcp 65001 >nul

powershell -Command "& {$text = Get-Clipboard; $lines = $text -split '\r?\n'; $result = @(); foreach ($line in $lines) { $chars = $line.ToCharArray(); $newLine = '·' + ($chars -join '·') + '·'; $result += $newLine }; $result -join \"`r`n\" | Set-Clipboard }"

echo 处理完成！剪贴板内容已更新。
pause
