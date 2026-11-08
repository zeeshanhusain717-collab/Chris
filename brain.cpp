// =============================================================================
// Z+2AI BRAIN — Offline Hindi Knowledge and Story Engine
// Architect / Creator: Zeeshan
// C++17; JNI bridge is enabled when compiled with __ANDROID__.
// =============================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <mutex>

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace z2ai {
using CodePoint = char32_t;

// Decode strictly enough to reject overlong, surrogate, and out-of-range
// encodings. Each malformed byte is replaced independently.
inline std::vector<CodePoint> utf8Decode(const std::string& text) {
    std::vector<CodePoint> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        CodePoint cp = 0;
        if (lead < 0x80) { length = 1; cp = lead; }
        else if ((lead & 0xE0) == 0xC0) { length = 2; cp = lead & 0x1F; }
        else if ((lead & 0xF0) == 0xE0) { length = 3; cp = lead & 0x0F; }
        else if ((lead & 0xF8) == 0xF0) { length = 4; cp = lead & 0x07; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + length > text.size()) { out.push_back(0xFFFD); ++i; continue; }
        bool valid = true;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (next & 0x3F);
        }
        const bool overlong = (length == 2 && cp < 0x80) ||
            (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000);
        if (!valid || overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(0xFFFD); ++i; continue;
        }
        out.push_back(cp);
        i += length;
    }
    return out;
}

inline void appendUtf8(std::string& out, CodePoint cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
inline std::string encodeUtf8(const std::vector<CodePoint>& points) {
    std::string out;
    out.reserve(points.size() * 3);
    for (CodePoint cp : points) appendUtf8(out, cp);
    return out;
}
inline std::string truncateCodePoints(const std::string& text, std::size_t maxPoints) {
    auto points = utf8Decode(text);
    if (points.size() > maxPoints) points.resize(maxPoints);
    return encodeUtf8(points);
}
inline std::string utf16ToUtf8(const std::u16string& text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (std::size_t i = 0; i < text.size(); ++i) {
        CodePoint cp = text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[++i] - 0xDC00);
        else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        appendUtf8(out, cp);
    }
    return out;
}
inline std::u16string utf8ToUtf16(const std::string& text) {
    const auto points = utf8Decode(text);
    std::u16string out;
    out.reserve(points.size());
    for (CodePoint cp : points) {
        if (cp <= 0xFFFF) out.push_back(static_cast<char16_t>(cp));
        else { cp -= 0x10000; out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
               out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))); }
    }
    return out;
}
inline bool edgePunctuation(CodePoint cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' ||
        cp == U'।' || cp == U'?' || cp == U'!' || cp == U',' || cp == U'.';
}
inline std::string trimText(const std::string& text) {
    auto points = utf8Decode(text);
    std::size_t first = 0, last = points.size();
    while (first < last && edgePunctuation(points[first])) ++first;
    while (last > first && edgePunctuation(points[last - 1])) --last;
    return encodeUtf8(std::vector<CodePoint>(points.begin() + first, points.begin() + last));
}
inline std::string normalizeKey(const std::string& text) {
    auto points = utf8Decode(text);
    std::vector<CodePoint> normalized;
    normalized.reserve(points.size());
    for (CodePoint cp : points) {
        if (cp == U'।' || cp == U'?' || cp == U'!' || cp == U',' || cp == U'.') continue;
        if (cp >= U'A' && cp <= U'Z') cp = cp - U'A' + U'a';
        normalized.push_back(cp);
    }
    std::size_t first = 0, last = normalized.size();
    while (first < last && (normalized[first] == U' ' || normalized[first] == U'\t')) ++first;
    while (last > first && (normalized[last - 1] == U' ' || normalized[last - 1] == U'\t')) --last;
    return encodeUtf8(std::vector<CodePoint>(normalized.begin() + first, normalized.begin() + last));
}
inline std::vector<std::string> tokenizeWords(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) { word = trimText(word); if (!word.empty()) words.push_back(word); }
    return words;
}

// Bounded Unicode-code-point Levenshtein distance; inputs are capped before use.
inline std::size_t levenshteinWithin(const std::string& lhs, const std::string& rhs,
                                    std::size_t limit) {
    const auto a = utf8Decode(lhs), b = utf8Decode(rhs);
    if (a.size() > 128 || b.size() > 128 ||
        (a.size() > b.size() && a.size() - b.size() > limit) ||
        (b.size() > a.size() && b.size() - a.size() > limit)) return limit + 1;
    std::vector<std::size_t> previous(b.size() + 1), current(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) previous[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = i;
        std::size_t rowMin = current[0];
        for (std::size_t j = 1; j <= b.size(); ++j) {
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                previous[j - 1] + (a[i - 1] == b[j - 1] ? 0u : 1u)});
            rowMin = std::min(rowMin, current[j]);
        }
        if (rowMin > limit) return limit + 1;
        previous.swap(current);
    }
    return previous[b.size()];
}
inline std::size_t fuzzyDistanceLimit(const std::string& key) {
    const auto n = utf8Decode(key).size();
    if (n == 0 || n > 64) return 0;
    return n <= 4 ? 1 : (n <= 8 ? 2 : 3);
}
inline std::string extractCoreKeyword(const std::string& input) {
    static const std::unordered_set<std::string> stopWords = {
        "क्या","कब","किस","कौन","कैसे","कहाँ","कहां","आप","तुम","मैं","मैंने",
        "इस","उस","और","लेकिन","अभी","यह","वह","से","पर","में","को","का","की",
        "है","हो","कर","बताओ","समझाओ","मुझे","के","लिए","what","when","where",
        "who","why","how","which","the","a","an","this","that","and","or","but",
        "is","are","was","were","to","of","in","on","for","with","at"
    };
    for (const auto& word : tokenizeWords(input)) {
        const auto key = normalizeKey(word);
        if (!key.empty() && stopWords.find(key) == stopWords.end()) return key;
    }
    const auto words = tokenizeWords(input);
    return words.empty() ? std::string() : normalizeKey(words.front());
}

// Twelve protagonists crossed with twelve situations produce 144 embedded,
// distinct combinations without carrying a large duplicated story corpus.
inline std::vector<std::string> buildNarratives() {
    static const std::array<const char*, 12> people = {{
        "ईमानदार किसान", "दयालु बच्चा", "समझदार कछुआ", "मेहनती चींटी",
        "साहसी हिरण", "वफादार कुत्ता", "बुद्धिमान उल्लू", "सहयोगी मछुआरा",
        "धैर्यवान माली", "सच्चा मित्र", "जिज्ञासु विद्यार्थी", "न्यायप्रिय मुखिया"
    }};
    static const std::array<const char*, 12> actions = {{
        "सूखे खेत में पानी बाँटा", "रास्ता भटके यात्री की सहायता की",
        "टूटा पुल मिलकर बनाया", "भूखे पक्षियों के लिए दाना रखा",
        "झगड़ते साथियों में न्याय किया", "कठिन अभ्यास जारी रखा",
        "मिली वस्तु उसके मालिक को लौटाई", "पेड़ लगाकर गाँव को छाया दी",
        "बाढ़ में पड़ोसियों को सुरक्षित जगह पहुँचाया", "अपनी गलती मानकर उसे सुधारा",
        "कमज़ोर साथी को धैर्य से सिखाया", "सबकी बात सुनकर साझा समाधान खोजा"
    }};
    static const std::array<const char*, 12> lessons = {{
        "साझा करने से सबका कल्याण होता है", "सच्चाई विश्वास की नींव है",
        "धैर्य कठिन लक्ष्य को संभव बनाता है", "मिलजुलकर काम करने से ताकत बढ़ती है",
        "दया से समाज में अपनापन बढ़ता है", "न्याय और निष्पक्षता जरूरी हैं",
        "प्रकृति की रक्षा हमारा कर्तव्य है", "गलतियों से सीखना बुद्धिमानी है",
        "नियमित परिश्रम सफलता लाता है", "संकट में सहायता करना सच्ची मानवता है",
        "ध्यान से सुनना सम्मान का रूप है", "ज्ञान बाँटने से सभी आगे बढ़ते हैं"
    }};
    std::vector<std::string> stories;
    stories.reserve(people.size() * actions.size());
    for (std::size_t i = 0; i < people.size(); ++i)
        for (std::size_t j = 0; j < actions.size(); ++j)
            stories.push_back(std::string("एक गाँव में ") + people[i] +
                " रहता था। एक दिन उसने " + actions[j] +
                ". उसके अच्छे काम से सभी प्रसन्न हुए। इस कहानी की सीख है कि " +
                lessons[(i + j) % lessons.size()] + "।");
    return stories;
}

inline const std::vector<std::pair<std::string, std::string>>& qaEntries() {
    static const std::vector<std::pair<std::string, std::string>> entries = {
        {"तुम्हारा नाम क्या है", "मेरा नाम Z+2AI है, जिसे ज़ीशान ने बनाया है।"},
        {"आपका नाम क्या है", "मेरा नाम Z+2AI है, जिसे ज़ीशान ने बनाया है।"},
        {"तुम्हें किसने बनाया है", "मुझे ज़ीशान ने बनाया है।"},
        {"आपके रचयिता कौन हैं", "मेरे रचयिता ज़ीशान हैं।"},
        {"क्या तुम हिंदी समझते हो", "हाँ, मैं हिंदी में लिखे प्रश्नों का उत्तर देने का प्रयास करता हूँ।"},
        {"तुम क्या कर सकते हो", "मैं स्थानीय ज्ञान, संबंध-शब्दकोश और हिंदी कहानियों से उत्तर दे सकता हूँ।"},
        {"धन्यवाद", "आपका स्वागत है।"},
        {"नमस्ते", "नमस्ते! बताइए, मैं आपकी क्या सहायता करूँ?"}
    };
    return entries;
}
class KnowledgeBase {
public:
    KnowledgeBase() {
        for (const auto& entry : qaEntries()) qa_.emplace(normalizeKey(entry.first), entry.second);
    }
    std::optional<std::string> findExact(const std::string& input) const {
        const auto it = qa_.find(normalizeKey(input));
        return it == qa_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    std::optional<std::string> findFuzzy(const std::string& input) const {
        const auto key = normalizeKey(input);
        const auto limit = fuzzyDistanceLimit(key);
        if (!limit) return std::nullopt;
        std::size_t best = limit + 1;
        const std::string* answer = nullptr;
        bool tied = false;
        for (const auto& entry : qa_) {
            const auto distance = levenshteinWithin(key, entry.first, limit);
            if (distance < best) { best = distance; answer = &entry.second; tied = false; }
            else if (distance == best) tied = true;
        }
        return answer && !tied && best <= limit ?
            std::optional<std::string>(*answer) : std::nullopt;
    }
private:
    std::unordered_map<std::string, std::string> qa_;
};

inline const std::unordered_map<std::string, std::string>& relationshipDictionary() {
    // Terms are grouped by relationship type; every alias resolves in O(1).
    static const std::unordered_map<std::string, std::string> dictionary = [] {
        std::unordered_map<std::string, std::string> d;
        const auto add = [&d](const char* terms, const char* meaning) {
            std::istringstream in(terms);
            std::string term;
            while (in >> term) d.emplace(normalizeKey(term), meaning);
        };
        add("माता माँ मम्मी mother", "माता का संबंध स्नेह, देखभाल और मार्गदर्शन से जुड़ा है; संवाद और सम्मान इसे मजबूत करते हैं।");
        add("पिता पिताजी पापा father", "पिता का संबंध सहयोग, देखभाल और मार्गदर्शन से जुड़ा है; विश्वास और सम्मान महत्वपूर्ण हैं।");
        add("भाई भैया भ्राता brother", "भाई सहोदर और सहयोगी संबंध है; अपनापन, संवाद और परस्पर सम्मान इसे मजबूत करते हैं।");
        add("बहन दीदी बहना sister", "बहन सहोदर और स्नेहपूर्ण संबंध है; विश्वास, बराबरी और सम्मान जरूरी हैं।");
        add("बेटा पुत्र son", "बेटा परिवार का सदस्य है; हर बच्चे को स्नेह, सुरक्षा, समान सम्मान और सीखने के अवसर चाहिए।");
        add("बेटी पुत्री daughter", "बेटी परिवार का सदस्य है; समान सम्मान, सुरक्षा और अवसर उसके अधिकार हैं।");
        add("पति जीवनसाथी husband", "जीवनसाथी का स्वस्थ संबंध आपसी सम्मान, सहमति, सुरक्षित सीमाओं और खुले संवाद पर टिकता है।");
        add("पत्नी जीवनसाथी wife", "जीवनसाथी का स्वस्थ संबंध आपसी सम्मान, सहमति, सुरक्षित सीमाओं और खुले संवाद पर टिकता है।");
        add("दादा दादाजी grandfather-paternal", "दादा परिवार के वरिष्ठ सदस्य और अनुभव के स्रोत हो सकते हैं; उनके साथ स्नेह और सम्मान से संवाद करें।");
        add("दादी दादीजी grandmother-paternal", "दादी परिवार की वरिष्ठ सदस्य और अनुभव तथा स्नेह का स्रोत हो सकती हैं; देखभाल और सम्मान जरूरी हैं।");
        add("नाना नानाजी grandfather-maternal", "नाना मातृ परिवार के वरिष्ठ सदस्य और मार्गदर्शक हो सकते हैं; अपनापन और सम्मान बनाए रखें।");
        add("नानी नानीजी grandmother-maternal", "नानी मातृ परिवार की वरिष्ठ सदस्य और स्नेह का स्रोत हो सकती हैं; देखभाल और सम्मान जरूरी हैं।");
        add("चाचा काका uncle-paternal", "चाचा पिता के भाई और विस्तृत परिवार के सदस्य होते हैं; रिश्ते में संवाद और सम्मान रखें।");
        add("चाची aunt-paternal", "चाची विस्तृत परिवार की सदस्य होती हैं; सहयोग, सम्मान और अपनापन रिश्ते को मजबूत करते हैं।");
        add("ताऊ ताऊजी uncle-elder-paternal", "ताऊ पिता के बड़े भाई होते हैं; परिवार में परस्पर सम्मान और सहयोग महत्वपूर्ण हैं।");
        add("ताई aunt-elder-paternal", "ताई विस्तृत परिवार की सदस्य हैं; आत्मीय संवाद और सम्मान से संबंध अच्छे रहते हैं।");
        add("मामा मामाजी uncle-maternal", "मामा माता के भाई और विस्तृत परिवार के सदस्य होते हैं; स्नेह और सम्मान से जुड़ाव बढ़ता है।");
        add("मामी aunt-maternal", "मामी मातृ परिवार की सदस्य हैं; अपनापन, संवाद और सम्मान रिश्ते को मजबूत करते हैं।");
        add("मौसा uncle-maternal-married", "मौसा विस्तृत मातृ परिवार के सदस्य होते हैं; रिश्तों में सौहार्द और सम्मान रखें।");
        add("मौसी aunt-maternal", "मौसी मातृ परिवार की सदस्य हैं; स्नेह, सहयोग और सम्मान उपयोगी हैं।");
        add("फूफा uncle-paternal-married", "फूफा विस्तृत पितृ परिवार के सदस्य होते हैं; सौहार्दपूर्ण संवाद संबंधों को बेहतर बनाता है।");
        add("बुआ aunt-paternal", "बुआ पिता की बहन और परिवार की सदस्य होती हैं; सम्मान और अपनापन बनाए रखें।");
        add("भतीजा nephew-brother", "भतीजा परिवार का युवा सदस्य है; बच्चों को सुरक्षित वातावरण, स्नेह और प्रोत्साहन चाहिए।");
        add("भतीजी niece-brother", "भतीजी परिवार की युवा सदस्य है; समान अवसर, स्नेह और सुरक्षा महत्वपूर्ण हैं।");
        add("भांजा nephew-sister", "भांजा विस्तृत परिवार का सदस्य है; मार्गदर्शन के साथ उसकी गरिमा और स्वतंत्रता का आदर करें।");
        add("भांजी niece-sister", "भांजी विस्तृत परिवार की सदस्य है; समान सम्मान और अवसर उसके लिए जरूरी हैं।");
        add("पोता grandson", "पोता परिवार की अगली पीढ़ी का सदस्य है; स्नेह, सुरक्षा और सीखने के मौके दें।");
        add("पोती granddaughter", "पोती परिवार की अगली पीढ़ी की सदस्य है; समान सम्मान, सुरक्षा और अवसर जरूरी हैं।");
        add("ससुर father-in-law", "ससुराल के रिश्ते आपसी सम्मान, स्पष्ट संवाद और स्वस्थ सीमाओं से बेहतर रहते हैं।");
        add("सास mother-in-law", "सास के साथ संबंध में सम्मान, संवाद और दोनों पक्षों की सहज सीमाएँ महत्वपूर्ण हैं।");
        add("देवर brother-in-law-husband-younger", "देवर विस्तृत परिवार का सदस्य है; मित्रवत व्यवहार के साथ परस्पर सम्मान रखें।");
        add("जेठ brother-in-law-husband-elder", "जेठ विस्तृत परिवार के सदस्य हैं; बराबरी, सम्मान और स्पष्ट संवाद अच्छा आधार हैं।");
        add("ननद sister-in-law-husband", "ननद विस्तृत परिवार की सदस्य हैं; अपनापन और एक-दूसरे की सीमाओं का सम्मान करें।");
        add("साला brother-in-law-wife", "साला विस्तृत परिवार का सदस्य है; सहयोग और सम्मानपूर्ण संवाद से रिश्ते मजबूत होते हैं।");
        add("साली sister-in-law-wife", "साली विस्तृत परिवार की सदस्य हैं; सम्मान, सौहार्द और सुरक्षित सीमाएँ बनाए रखें।");
        add("दोस्त दोस्ती मित्र friend", "दोस्ती विश्वास, सहानुभूति और एक-दूसरे की सीमाओं का आदर करने से बढ़ती है।");
        add("मित्रता friendship", "मित्रता में भरोसा, ईमानदार संवाद, परस्पर सहयोग और व्यक्तिगत सीमाओं का सम्मान जरूरी है।");
        add("दुश्मन शत्रु enemy", "विरोध की स्थिति में भी शांत संवाद, सुरक्षित सीमाएँ और निष्पक्षता उपयोगी हैं; खतरे में मदद लें।");
        add("पड़ोसी पड़ोस neighbor", "पड़ोसियों के साथ सहयोग और सम्मान से समुदाय अधिक सुरक्षित और सुखद बनता है।");
        add("सहकर्मी colleague", "सहकर्मियों के साथ निष्पक्षता, शिष्ट संवाद और सहयोग से कार्यस्थल बेहतर बनता है।");
        add("साथी partner", "साथी के साथ स्वस्थ संबंध सहमति, समानता, भरोसे और सम्मानपूर्ण संवाद पर आधारित होता है।");
        add("प्रेमी प्रेमिका beloved", "प्रेमपूर्ण संबंध में सहमति, ईमानदारी, सुरक्षा और एक-दूसरे की स्वतंत्रता का सम्मान आवश्यक है।");
        add("राजा सम्राट ruler", "शासक का आदर्श कर्तव्य न्याय करना और सभी नागरिकों के कल्याण तथा अधिकारों की रक्षा करना है।");
        add("रानी महारानी queen", "नेतृत्व में न्याय, जवाबदेही और सभी लोगों के कल्याण को प्राथमिकता देना चाहिए।");
        add("प्रजा नागरिक citizen", "समाज के हर नागरिक को समान गरिमा, न्याय और अपने विचार रखने का अधिकार मिलना चाहिए।");
        add("सेवक कर्मचारी worker", "हर काम सम्मानजनक है; कर्मचारियों के साथ गरिमा, निष्पक्षता और सुरक्षित व्यवहार जरूरी है।");
        add("मालिक नियोक्ता employer", "जिम्मेदार नेतृत्व निष्पक्ष अवसर, सुरक्षित कार्यस्थल और सम्मानपूर्ण व्यवहार सुनिश्चित करता है।");
        add("गुरु अध्यापक mentor", "गुरु ज्ञान और मार्गदर्शन देते हैं; सीखने में प्रश्न पूछना, अभ्यास और पारस्परिक सम्मान जरूरी हैं।");
        add("शिष्य विद्यार्थी छात्र student", "विद्यार्थी जिज्ञासा, नियमित अभ्यास और विनम्रता से सीखता है; गलती सीखने का अवसर है।");
        add("शिक्षक शिक्षिका teacher", "शिक्षक सीखने में मार्गदर्शन करते हैं; प्रोत्साहन, निष्पक्षता और संवाद से समझ बढ़ती है।");
        add("डॉक्टर चिकित्सक doctor", "चिकित्सक स्वास्थ्य-सेवा देते हैं; सवाल पूछें और व्यक्तिगत उपचार के लिए योग्य पेशेवर से सलाह लें।");
        add("रोगी मरीज patient", "रोगी को गरिमा, सहानुभूति और गोपनीयता के साथ देखभाल मिलनी चाहिए।");
        add("नेता नेतृत्व leader", "अच्छा नेतृत्व सुनता है, जवाबदेही स्वीकारता है और सबके हित तथा समानता का ध्यान रखता है।");
        add("समाज समुदाय community", "समाज सहयोग, समान गरिमा, जिम्मेदारी और न्याय से मजबूत बनता है।");
        add("परिवार कुटुंब family", "परिवार में संवाद, देखभाल, सम्मान और सभी सदस्यों की सुरक्षित सीमाएँ महत्वपूर्ण हैं।");
        add("बच्चा बालक बालिका child", "बच्चों को स्नेह, सुरक्षा, सम्मान और सीखने ��था खेलने के अवसर चाहिए।");
        add("वृद्ध बुजुर्ग elder", "वरिष्ठ लोगों के अनुभव का आदर करें और उनकी स्वतंत्रता, गरिमा तथा आवश्यकताओं का ध्यान रखें।");
        add("अतिथि मेहमान guest", "अतिथि के साथ सद्भाव और सम्मान से पेश आएँ; मेजबान और अतिथि दोनों की सहजता महत्वपूर्ण है।");
        add("मेजबान host", "अच्छा मेजबान अतिथि का सम्मान करता है और सभी की सहजता तथा सीमाओं का ध्यान रखता है।");
        add("सहायक मददगार helper", "सहायता संवेदनशीलता से दें, सामने वाले की सहमति और आत्मसम्मान का ध्यान रखें।");
        add("प्रतिद्वंद्वी competitor", "प्रतिस्पर्धा में निष्प��्षता, खेलभावना और दूसरों के प्रयास का सम्मान बनाए रखें।");
        add("नागरिक नागरिकता people", "नागरिक समाज का महत्वपूर्ण हिस्सा हैं; जिम्मेदारी, समान अधिकार और सहभागिता लोकतंत्र को मजबूत करते हैं।");
        add("समुदायजन साथीजन fellow", "सामुदायिक जीवन में सहयोग, समावेश और एक-दूसरे की गरिमा का सम्मान आवश्यक है।");
        return d;
    }();
    return dictionary;
}
inline std::string domainLookup(const std::string& token) {
    const auto key = normalizeKey(token);
    const auto& relations = relationshipDictionary();
    const auto relation = relations.find(key);
    if (relation != relations.end()) return relation->second;
    static const std::unordered_map<std::string, std::string> facts = {
        {"रोटी", "रोटी आटे से बनने वाला सामान्य भोजन है; संतुलित आहार में विविधता रखें।"},
        {"दूध", "दूध पोषक तत्वों का स्रोत है; आहार संबंधी जरूरतें व्यक्ति के अनुसार बदलती हैं।"},
        {"शेर", "शेर एक बड़ा जंगली बिल्ली-प्रजाति का स्तनधारी है।"},
        {"गाय", "गाय पालतू स्तनधारी पशु है; पशुओं के साथ दयालु व्यवहार करें।"},
        {"तोता", "तोता रंगीन पक्षी है; पक्षियों के प्राकृतिक आवास का संरक्षण जरूरी है।"},
        {"खुशी", "खुशी प्रसन्नता का भाव है; भावनाओं को पहचानना और साझा करना सहायक हो सकता है।"},
        {"पानी", "पानी जीवन के लिए आवश्यक है; इसे स्वच्छ रखें और जिम्मेदारी से उपयोग करें���"},
        {"पेड़", "पेड़ छाया और स्वच्छ पर्यावरण में योगदान देते हैं; पौधों की देखभाल करें।"},
        {"सूरज", "सूर्य पृथ्वी को प्रकाश और ऊष्मा देता है; बाहर जाते समय धूप से बचाव करें।"},
        {"चाँद", "चंद्रमा पृथ्वी का प्राकृतिक उपग्रह है और रात के आकाश में दिखाई देता है।"},
        {"पृथ्वी", "पृथ्वी हमारा साझा घर है; संसाधनों का संरक्षण और स्वच्छता सबकी जिम्मेदारी है।"},
        {"किताब", "किताबें ज्ञान और कल्पना का स्रोत हैं; नियमित पढ़ने से भाषा और समझ विकसित होती है।"},
        {"स्कूल", "स्कूल सीखने, अभ्यास और सहयोग की जगह है; प्रश्न पूछना सीखने का अच्छा तरीका है।"},
        {"समय", "समय का संतुलित उपयोग करें; काम, आराम और सीखने के लिए जगह रखना उपयोगी है।"},
        {"परिश्रम", "नियमित परिश्रम और छोटे लक्ष्य कठिन काम को पूरा करने में मदद करते हैं।"},
        {"सत्य", "सच्चाई भरोसे की नींव है; कठिन बात भी संवेदनशीलता और ईमानदारी से कहें।"},
        {"दया", "दया का अर्थ दूसरों की परिस्थिति समझना और सम्मानपूर्वक सहायता करना है।"},
        {"न्याय", "न्याय में निष्पक्षता, समान गरिमा और हर पक्ष की बात सुनना महत्वपूर्ण है।"},
        {"कहानी", "कहानियाँ कल्पना और सीख साझा करती हैं; बताइए, आप किस विषय पर कहानी चाहते हैं?"}
    };
    const auto fact = facts.find(key);
    return fact == facts.end() ? std::string() : fact->second;
}

class TrigramModel {
public:
    void train(const std::vector<std::string>& stories) {
        for (const auto& story : stories) {
            const auto words = tokenizeWords(story);
            for (std::size_t i = 0; i + 2 < words.size(); ++i)
                ++model_[words[i]][words[i + 1]][words[i + 2]];
        }
    }
    std::string generateSeeded(std::mt19937& rng, int maxWords,
                               const std::string& seed) const {
        // Always retain the requested topic as the visible seed, even when it
        // is not in the compact training vocabulary.
        std::string output = seed;
        if (model_.empty()) return output + " पर कहानी में पात्र ने धैर्य और सहयोग से समस्या का हल खोजा।";
        std::string w1, w2;
        const auto seedIt = model_.find(seed);
        if (seedIt != model_.end() && !seedIt->second.empty()) {
            std::vector<std::string> choices;
            std::vector<double> weights;
            for (const auto& item : seedIt->second) {
                choices.push_back(item.first);
                std::size_t count = 0;
                for (const auto& next : item.second) count += static_cast<std::size_t>(next.second);
                weights.push_back(static_cast<double>(count));
            }
            std::discrete_distribution<std::size_t> pick(weights.begin(), weights.end());
            w1 = seed;
            w2 = choices[pick(rng)];
            output += " " + w2;
        } else {
            auto first = model_.begin();
            if (first == model_.end() || first->second.empty())
                return output + " पर एक छोटी सीख: धैर्य और सहयोग से कठिनाई आसान होती है।";
            w1 = first->first;
            w2 = first->second.begin()->first;
            output += " पर " + w1 + " " + w2;
        }
        for (int step = 0; step < maxWords; ++step) {
            const auto one = model_.find(w1);
            if (one == model_.end()) break;
            const auto two = one->second.find(w2);
            if (two == one->second.end() || two->second.empty()) break;
            std::vector<std::string> choices;
            std::vector<double> weights;
            for (const auto& item : two->second) {
                choices.push_back(item.first);
                weights.push_back(static_cast<double>(item.second));
            }
            std::discrete_distribution<std::size_t> pick(weights.begin(), weights.end());
            const std::string next = choices[pick(rng)];
            output += " " + next;
            w1 = w2; w2 = next;
        }
        return output;
    }
private:
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unordered_map<std::string, int>>> model_;
};

struct ConversationTurn { std::string user; std::string assistant; };
class ConversationMemory {
public:
    static constexpr std::size_t kMaxTurns = 4;
    static constexpr std::size_t kMaxUserCodePoints = 256;
    static constexpr std::size_t kMaxAnswerCodePoints = 1024;
    void remember(const std::string& user, const std::string& assistant) {
        if (turns_.size() >= kMaxTurns) turns_.pop_front();
        turns_.push_back({truncateCodePoints(user, kMaxUserCodePoints),
                          truncateCodePoints(assistant, kMaxAnswerCodePoints)});
    }
private:
    std::deque<ConversationTurn> turns_;
};

class Brain {
public:
    Brain() : stories_(buildNarratives()) { model_.train(stories_); }
    std::string process(const std::string& raw) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string input = trimText(truncateCodePoints(raw, ConversationMemory::kMaxUserCodePoints));
        if (input.empty()) return "कुछ लिखिए, मैं सुन रहा हूँ।";
        std::string response;
        if (normalizeKey(input) == "बाहर" || normalizeKey(input) == "exit") {
            response = "अलविदा! Z+2AI से बात करने के लिए धन्यवाद।";
        } else if (const auto exact = kb_.findExact(input)) {
            response = *exact;
        } else if (const auto fuzzy = kb_.findFuzzy(input)) {
            response = *fuzzy;
        } else {
            for (const auto& word : tokenizeWords(input)) {
                response = domainLookup(word);
                if (!response.empty()) break;
            }
            if (response.empty()) {
                const std::string seed = extractCoreKeyword(input);
                if (seed.empty()) response = "Z+2AI आपकी बात समझने का प्रयास कर रहा है। कृपया कोई विषय या प्रश्न लिखें।";
                else {
                    std::uint32_t hash = 2166136261u;
                    for (unsigned char byte : seed) { hash ^= byte; hash *= 16777619u; }
                    std::mt19937 rng(hash);
                    const std::string body = model_.generateSeeded(rng, 12, seed);
                    response = "Z+2AI विषय: " + seed + "।\nएक छोटी सीख: " + body +
                        "\nशिक्षा: धैर्य, सहयोग और निरंतर प्रयास से कठिनाइयाँ आसान होती हैं।";
                }
            }
        }
        response = truncateCodePoints(response, ConversationMemory::kMaxAnswerCodePoints);
        memory_.remember(input, response);
        return response.empty() ? "मैं आपकी सहायता के लिए तैयार हूँ।" : response;
    }
private:
    KnowledgeBase kb_;
    std::vector<std::string> stories_;
    TrigramModel model_;
    ConversationMemory memory_;
    std::mutex mutex_;
};
inline Brain& brain() { static Brain instance; return instance; }
} // namespace z2ai

#ifdef __ANDROID__
extern "C" JNIEXPORT jstring JNICALL
Java_com_zeeshan_ai_MainActivity_nativeGetAIResponse(JNIEnv* env, jobject, jstring input) {
    if (env == nullptr) return nullptr;
    std::string result;
    try {
        std::u16string text;
        if (input != nullptr) {
            const jsize length = env->GetStringLength(input);
            const jchar* chars = env->GetStringChars(input, nullptr);
            if (chars != nullptr) {
                text.reserve(static_cast<std::size_t>(std::min<jsize>(length, 256)) + 1);
                std::size_t points = 0;
                for (jsize i = 0; i < length && points < 256; ++i, ++points) {
                    const char16_t unit = static_cast<char16_t>(chars[i]);
                    text.push_back(unit);
                    if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < length &&
                        chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF)
                        text.push_back(static_cast<char16_t>(chars[++i]));
                }
                env->ReleaseStringChars(input, chars);
            }
        }
        result = z2ai::brain().process(z2ai::utf16ToUtf8(text));
    } catch (...) {
        result = "मेमोरी या सिस्टम की समस्या आई। कृपया छोटा प्रश्न दोबारा लिखें।";
    }
    try {
        const std::u16string output = z2ai::utf8ToUtf16(
            z2ai::truncateCodePoints(result, z2ai::ConversationMemory::kMaxAnswerCodePoints));
        return env->NewString(reinterpret_cast<const jchar*>(output.data()),
                              static_cast<jsize>(output.size()));
    } catch (...) {
        const char16_t fallback[] = u"उत्तर उपलब्ध नहीं है।";
        return env->NewString(reinterpret_cast<const jchar*>(fallback),
                              static_cast<jsize>(sizeof(fallback) / sizeof(fallback[0]) - 1));
    }
}
#endif
