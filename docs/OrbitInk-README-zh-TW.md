# OrbitInk

BookOrbit firmware for Xteink X3。

這是 CrossFade／CrossPoint 衍生韌體，也整合 CrossInk v1.6.1 基底；
保留各上游作者的 LICENSE、NOTICE 及元件授權。本版本僅供 Xteink X3，
請勿安裝到 X4 或其他機型。

設定 → 系統 → 更新頻道可選「穩定版」或「每夜版（實驗性）」，
預設穩定版，選項會儲存在 SD 設定。選擇頻道不會自動下載或刷寫；
仍須執行檢查更新並在裝置確認。每夜版可能不穩定，請先備份。
切回較舊的穩定版會另外顯示降版確認，預設取消。

無線更新使用既有 P-256 金鑰驗證簽章，下載後核對大小與 SHA-256，
通過檢查才寫入非使用中的分割區。也可使用網頁手動上傳 .bin，
上傳與取消不會刷寫，實際安裝另需裝置確認。
更新保留 SD 上的書籍、閱讀紀錄、設定及字型。
未宣稱安全開機、硬體防降版、通用回復能力或已完成實機刷寫驗收；
回復能力取決於裝置 bootloader，斷電仍有風險。
既有 TLS 連線未驗證伺服器憑證，下載真實性依賴簽章及雜湊。

此公開套件**不含七個選用的 SD 中文字型**。現有使用者請保留 SD
`/fonts` 中原已安裝的 GuanKiapTsingKhai 14、18、20、22、24、26、28pt
字型。繁體中文介面／閱讀設定仍依賴既有字型；新裝置若沒有字型，
不要預期本套件會安裝它們。未取得精確字型授權前不另行散布。

原始碼根目錄的 MIT 授權不涵蓋所有連結元件。wolfSSL 隨附 GPLv2／
GPLv2-or-later 或商業授權條款；本開源交付保留其實際 COPYING、
LICENSING 及必要對應原始碼，不以根目錄 MIT 取代元件條款。
整合後的韌體二進位以 GPLv3 散布，採用 wolfSSL 的「或任何較新版本」
選項，以相容連結的 Apache-2.0 元件；實際 GPLv3 條文見
`licenses/project/LICENSES/COPYING.GPL3`。各原始檔仍保留自己的授權。
內建字型亦保留各自實際授權。對應原始碼及授權文件在
`corresponding-source.tar.gz` 與 `orbitink.zip` 的 `licenses/` 中。
它們包含此二進位所用建置輸入，沒有私人 Git 歷史或簽章私鑰。

建置：@BUILD@
來源提交：@HEAD@
韌體 SHA-256：@SHA256@

穩定版僅在 Edison 明確要求發版時建立。每夜版由受信任 main 推送產生，
不會取代穩定版 latest。原 BookOrbit-X3 build 1 仍可下載；
重新命名後保留舊網址重新導向，相容性須由發佈者匿名下載驗證。
