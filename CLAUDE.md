# CLAUDE.md

CCA 的開發規則只寫在一個地方，讓 Claude、GPT 和本地審查員看到同一份：

@AGENTS.md

## 給 Claude Code 的補充

- 你在 CCA 群組裡通常是撰寫者。交出前跑 `python tools\check_style.py --format --js` 和相關測試，回覆裡附上結果。
- Bash 工具在這台電腦是 Git Bash：heredoc 裡的 Python 字串 `'\n'` 可能被轉成真的換行，改 C++ 或 JS 時優先用編輯工具，不要用 Python 腳本做文字替換。
- `build.bat` 要從 PowerShell 用 `cmd /c ".\build.bat test"` 執行。
