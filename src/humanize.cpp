#include "humanize.hpp"

#include "util.hpp"

namespace {

const size_t kMaxExtra = 8000;  // bytes; the settings field allows 2000 characters

// Drawn from Wikipedia's "Signs of AI writing" field guide, designers' write-ups
// of "AI slop" UI tells (indigo gradients, Inter, three-card rows), and zh-TW
// notes on 翻譯腔 / AI 腔. They are tells, not proof, so the rule asks for a
// reason behind each choice rather than banning things outright.
const char* kRules =
    "# 去 AI 化（強制）\n"
    "使用者開啟了去 AI 化：你做出來的網頁、介面、美術素材和文字，不能讓人一看就覺得是 AI 做的。"
    "這條規則和需求一樣重要；只有使用者明確指定的風格可以例外。\n\n"
    "動手前先從專案本身決定方向：產品用途、目標使用者、既有的品牌色和字體、現有程式碼與文案的風格。"
    "專案已經有設計系統就沿用，不要自己換一套。每個視覺選擇都要說得出理由，說不出理由的通常就是預設值。\n\n"
    "## 網頁與介面\n"
    "避免：\n"
    "- 沒有理由的紫色、靛藍到藍紫漸層（Tailwind indigo-500 那一類預設色），以及漸層文字、漸層按鈕、發光陰影、層層疊疊的玻璃擬態。\n"
    "- 拿 Inter、Roboto、Arial 或系統預設字體當主視覺字體卻沒有任何考量；中文頁面要另外挑中文字體與字重。\n"
    "- 公版版型：置中大標＋副標＋兩顆按鈕的 hero、一排三張等寬圓角卡片、每個區塊一樣的上下間距、所有元素都大圓角加柔和陰影。\n"
    "- 每個標題旁都配 emoji 或通用細線圖示；裝飾用的浮動光球、模糊色塊、和內容無關的幾何或星空背景。\n"
    "- 假資料與空話：John Doe、Acme、Lorem ipsum、編造的評價、數據和客戶 logo；"
    "「打造」「賦能」「釋放潛力」「無縫」「一站式」「新一代」「Get Started」這類行銷詞。\n"
    "應該：用從內容推導出來的有限色盤和字級；版面要有主次和節奏（不對稱、密度變化）；"
    "圖示、插圖和文案貼合這個產品；對齊、間距、hover／focus 狀態、空狀態、錯誤訊息這些細節要像有人真的用過。\n\n"
    "## 美術與素材（SVG、圖示、插畫、配色、給圖像模型的提示詞）\n"
    "- 避免塑膠般過度光滑的質感、沒來由的霓虹與發光、完美對稱、到處都是漸層、和主題無關的抽象流體或電路板背景。\n"
    "- 線條粗細、圓角、透視和光源要一致；風格要說得出參考了什麼，保留有意圖的不完美和手作感。\n"
    "- 給圖像模型的提示詞要寫具體的媒材、構圖、光線和參考風格，不要只堆「高品質、超精細、8K、大師級」。\n\n"
    "## 文字（介面文案、說明、README、產品描述、commit 訊息）\n"
    "- 不要用套話：「值得注意的是」「總而言之」「綜上所述」「在這個……的時代」「讓我們一起」「希望這對你有幫助」，"
    "「不僅……更……」「不是……而是……」這類對照句，「里程碑」「至關重要」「深遠影響」這類誇大意義的詞；"
    "英文避免 delve、tapestry、seamless、elevate、unlock、testament、vibrant、\"not just X, but Y\"。\n"
    "- 不要什麼都湊成三點，不要每段一樣長，不要每段結尾補一句總結；少用粗體、破折號、冒號清單和 emoji。\n"
    "- 用具體事實取代形容詞：寫出數字、名稱和操作步驟。能用「是」「有」就不要用「作為」「擁有」「提供」；翻譯腔的長句拆短。\n"
    "- 語氣跟專案現有的文字一致；不確定的事直接說不確定。\n\n"
    "交付前照上面的清單自己檢查一遍，像 AI 預設值的地方改掉再交。";

}  // namespace

std::string humanizeInstructions(const std::string& extra) {
    std::string text = kRules;
    std::string mine = util::trim(extra);
    if (!mine.empty()) text += "\n\n## 使用者另外的要求\n" + util::truncateUtf8(mine, kMaxExtra);
    return text;
}

std::string humanizeReviewNote(const std::string& extra) {
    std::string text = "去 AI 化檢查：使用者開啟了去 AI 化。請一併檢查這一輪做出來的網頁、介面、美術素材和文字有沒有 AI 預設感，"
           "例如沒有理由的紫色漸層、置中 hero 加三張卡片的公版版型、沒有考量就用 Inter、標題配 emoji、假資料、"
           "行銷空話、「值得注意的是」「不僅……更……」這類套話、過度排比和粗體；美術素材檢查過度光滑、無來由的霓虹發光，"
           "以及媒材、構圖、光線不明確的圖像提示詞。有的話當作需要修正的問題處理。"
           "沿用專案既有設計系統；使用者明確指定的風格可以例外，不要只憑單一特徵就要求改動。";
    std::string mine = util::trim(extra);
    if (!mine.empty()) text += "\n\n## 使用者另外的要求\n" + util::truncateUtf8(mine, kMaxExtra);
    return text;
}
