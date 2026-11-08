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

// UTF-8 decoder replaces malformed sequences and rejects overlong, surrogate,
// and out-of-range encodings. It advances one byte on malformed input.
inline std::vector<CodePoint> utf8Decode(const std::string& text) {
    std::vector<CodePoint> out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
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
                              (length == 3 && cp < 0x800) ||
                              (length == 4 && cp < 0x10000);
        if (!valid || overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(0xFFFD);
            ++i;
            continue;
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
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[++i] - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
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
        else {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

inline bool edgePunctuation(CodePoint cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' ||
           cp == U'।' || cp == U'?' || cp == U'!' || cp == U',' || cp == U'.';
}
inline std::string trimText(const std::string& text) {
    auto points = utf8Decode(text);
    std::size_t begin = 0, end = points.size();
    while (begin < end && edgePunctuation(points[begin])) ++begin;
    while (end > begin && edgePunctuation(points[end - 1])) --end;
    return encodeUtf8(std::vector<CodePoint>(points.begin() + begin, points.begin() + end));
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
    std::size_t begin = 0, end = normalized.size();
    while (begin < end && (normalized[begin] == U' ' || normalized[begin] == U'\t')) ++begin;
    while (end > begin && (normalized[end - 1] == U' ' || normalized[end - 1] == U'\t')) --end;
    return encodeUtf8(std::vector<CodePoint>(normalized.begin() + begin, normalized.begin() + end));
}
inline std::vector<std::string> tokenizeWords(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::string> result;
    std::string word;
    while (input >> word) {
        word = trimText(word);
        if (!word.empty()) result.push_back(word);
    }
    return result;
}

inline std::size_t levenshteinWithin(const std::string& lhs, const std::string& rhs,
                                    std::size_t limit) {
    const auto a = utf8Decode(lhs), b = utf8Decode(rhs);
    if (a.size() > 128 || b.size() > 128) return limit + 1;
    if (a.size() > b.size() + limit || b.size() > a.size() + limit) return limit + 1;
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
    const auto length = utf8Decode(key).size();
    if (length == 0 || length > 64) return 0;
    return length <= 4 ? 1 : (length <= 8 ? 2 : 3);
}

inline std::string extractCoreKeyword(const std::string& input) {
    static const std::unordered_set<std::string> stopWords = {
        "क्या","कब","किस","कौन","कैसे","कहाँ","कहां","आप","तुम","मैं","मैंने",
        "इस","उस","और","लेकिन","तब","अभी","यह","वह","से","पर","में","को",
        "का","की","है","हो","कर","बताओ","समझाओ","मुझे","के","लिए",
        "what","when","where","who","why","how","which","the","a","an",
        "this","that","and","or","but","is","are","was","were","to","of",
        "in","on","for","with","at"
    };
    for (const auto& token : tokenizeWords(input)) {
        const auto key = normalizeKey(token);
        if (!key.empty() && stopWords.find(key) == stopWords.end()) return key;
    }
    const auto tokens = tokenizeWords(input);
    return tokens.empty() ? std::string() : normalizeKey(tokens.front());
}

// The narrative matrix combines ten distinct protagonists with ten distinct
// situations. Its 100 entries are generated once, entirely from embedded data.
inline std::vector<std::string> buildNarratives() {
    static const std::array<std::string, 10> people = {{
        "ईमानदार किसान", "दयालु बच्चा", "समझदार कछुआ", "मेहनती चींटी", "साहसी हिरण",
        "वफादार कुत्ता", "बुद्धिमान उल्लू", "सहयोगी मछुआरा", "धैर्यवान माली", "सच्चा मित्र"
    }};
    static const std::array<std::string, 10> situations = {{
        "सूखे खेत में पानी बाँटा", "रास्ता भटके यात्री की सहायता की", "टूटा पुल मिलकर बनाया",
        "भूखे पक्षियों के लिए दाना रखा", "झगड़ते साथियों में न्याय किया", "कठिन अभ्यास जारी रखा",
        "मिली हुई वस्तु उसके मालिक को लौटाई", "पेड़ लगाकर गाँव को छाया दी",
        "बाढ़ में पड़ोसियों को सुरक्षित जगह पहुँचाया", "अपनी गलती मानकर उसे सुधारा"
    }};
    static const std::array<std::string, 10> lessons = {{
        "साझा करने से सबका कल्याण होता है", "सच्चाई विश्वास की नींव है",
        "धैर्य कठिन लक्ष्य को संभव बनाता है", "मिलजुलकर काम करने से ताकत बढ़ती है",
        "दया से समाज में अपनापन बढ़ता है", "न्याय और निष्पक्षता जरूरी हैं",
        "प्रकृति की रक्षा हमारा कर्तव्य है", "गलतियों से सीखना बुद्धिमानी है",
        "नियमित परिश्रम सफलता लाता है", "संकट में सहायता करना सच्ची मानवता है"
    }};
    std::vector<std::string> stories;
    stories.reserve(100);
    for (std::size_t i = 0; i < people.size(); ++i) {
        for (std::size_t j = 0; j < situations.size(); ++j) {
            stories.push_back("एक गाँव में " + people[i] + " रहता था। एक दिन उसने " +
                situations[j] + ". उसके अच्छे काम से सभी प्रसन्न हुए। इस कहानी की सीख है कि " +
                lessons[(i + j) % lessons.size()] + "।");
        }
    }
    return stories;
}

inline const std::vector<std::pair<std::string, std::string>>& qaEntries() {
    static const std::vector<std::pair<std::string, std::string>> entries = {
        {"तुम्हारा नाम क्या है", "मेरा नाम Z+2AI है, जिसे ज़ीशान ने बनाया है।"},
        {"आपका नाम क्या है", "मेरा नाम Z+2AI है, जिसे ज़ीशान ने बनाया है।"},
        {"तुम्हें किसने बनाया है", "मुझे ज़ीशान ने बनाया है।"},
        {"आपके रचयिता कौन हैं", "मेरे रचयिता ज़ीशान हैं।"},
        {"क्या तुम हिंदी समझते हो", "हाँ, मैं हिंदी में लिखे प्रश्नों का उत्तर देने का प्रयास करता हूँ।"},
        {"तुम क्या कर सकते हो", "मैं अपने स्थानीय ज्ञान, संबंध-शब्दकोश और हिंदी कहानियों से उत्तर दे सकता हूँ।"},
        {"धन्यवाद", "आपका स्वागत है।"},
        {"नमस्ते", "नमस्ते! बताइए, मैं आपकी क्या सहायता करूँ?"}
    };
    return entries;
}

class KnowledgeBase {
public:
    KnowledgeBase() {
        for (const auto& item : qaEntries()) qa_.emplace(normalizeKey(item.first), item.second);
    }
    std::optional<std::string> find(const std::string& input) const {
        const auto key = normalizeKey(input);
        const auto exact = qa_.find(key);
        if (exact != qa_.end()) return exact->second;
        const auto limit = fuzzyDistanceLimit(key);
        if (limit == 0) return std::nullopt;
        std::size_t best = limit + 1;
        const std::string* answer = nullptr;
        bool tied = false;
        for (const auto& entry : qa_) {
            const auto distance = levenshteinWithin(key, entry.first, limit);
            if (distance < best) { best = distance; answer = &entry.second; tied = false; }
            else if (distance == best) tied = true;
        }
        if (answer && !tied && best <= limit) return *answer;
        return std::nullopt;
    }
private:
    std::unordered_map<std::string, std::string> qa_;
};

inline const std::unordered_map<std::string, std::string>& relationshipDictionary() {
    static const std::unordered_map<std::string, std::string> dictionary = {
        {"माता", "माता परिवार में स्नेह, देखभाल और मार्गदर्शन का महत्वपूर्ण संबंध है।"},
        {"माँ", "माँ परिवार में स्नेह, देखभाल और मार्गदर्शन का महत्वपूर्ण संबंध है।"},
        {"पिता", "पिता परिवार में सहयोग, सुरक्षा और मार्गदर्शन का महत्वपूर्ण संबंध है।"},
        {"भाई", "भाई सहोदर और सहयोगी संबंध है; आपसी सम्मान से यह रिश्ता मजबूत होता है।"},
        {"बहन", "बहन सहोदर और स्नेहपूर्ण संबंध है; विश्वास और सम्मान इसे मजबूत करते हैं।"},
        {"बेटा", "बेटा परिवार का प्रिय सदस्य है; उसे स्नेह और जिम्मेदारी दोनों की जरूरत होती है।"},
        {"बेटी", "बेटी परिवार का प्रिय सदस्य है; समान सम्मान और अवसर उसके अधिकार हैं।"},
        {"पति", "पति जीवनसाथी है; स्वस्थ संबंध आपसी सम्मान, सहमति और संवाद पर टिकता है।"},
        {"पत्नी", "पत्नी जीवनसाथी है; स्वस्थ संबंध आपसी सम्मान, सहमति और संवाद पर टिकता है।"},
        {"दादा", "दादा परिवार के वरिष्ठ सदस्य और अनुभव के स्रोत हो सकते हैं।"},
        {"दादी", "दादी परिवार की वरिष्ठ सदस्य और अनुभव तथा स्नेह का स्रोत हो सकती हैं।"},
        {"नाना", "नाना मातृ परिवार के वरिष्ठ सदस्य और मार्गदर्शक हो सकते हैं।"},
        {"नानी", "नानी मातृ परिवार की वरिष्ठ सदस्य और स्नेह का स्रोत हो सकती हैं।"},
        {"चाचा", "चाचा पिता के भाई और विस्तृत परिवार के सदस्य होते हैं।"},
        {"चाची", "चाची विस्तृत परिवार की सदस्य होती हैं; रिश्ते में सम्मान और अपनापन रखें।"},
        {"मामा", "मामा माता के भाई और विस्तृत परिवार के सदस्य होते हैं।"},
        {"मामी", "मामी विस्तृत परिवार की सदस्य होती हैं; रिश्ते में सम्मान और अपनापन रखें।"},
        {"दोस्त", "दोस्ती विश्वास, सम्मान और एक-दूसरे की सीमाओं का आदर करने से बढ़ती है।"},
        {"मित्र", "मित्रता विश्वास, सहानुभूति और परस्पर सम्मान पर आधारित होती है।"},
        {"दुश्मन", "विरोध की स्थिति में भी शांत संवाद, सुरक्षित सीमाएँ और निष्पक्षता उपयोगी हैं।"},
        {"राजा", "राजा का आदर्श कर्तव्य न्याय करना और प्रजा के कल्याण की रक्षा करना है।"},
        {"प्रजा", "प्रजा समाज के नागरिकों का समूह है; सभी को सम्मान और न्याय मिलना चाहिए।"},
        {"सेवक", "सेवक का कार्य सम्मानजनक है; हर व्यक्ति के साथ गरिमा और निष्पक्षता से व्यवहार करें।"},
        {"गुरु", "गुरु ज्ञान और मार्गदर्शन देते हैं; सीखने में प्रश्न पूछना और सम्मान जरूरी है।"},
        {"शिष्य", "शिष्य सीखने वाला विद्यार्थी है; जिज्ञासा, अभ्यास और विनम्रता उपयोगी गुण हैं।"},
        {"परिवार", "परिवार में संवाद, देखभाल, सम्मान और सुरक्षित सीमाएँ महत्वपूर्ण हैं।"},
        {"पड़ोसी", "पड़ोसियों के साथ सहयोग और सम्मान से समुदाय अधिक सुरक्षित और सुखद बनता है।"},
        {"बच्चा", "बच्चों को स्नेह, सुरक्षा, सम्मान और सीखने के अवसर चाहिए।"},
        {"शिक्षक", "शिक्षक सीखने में मार्गदर्शन करते हैं; संवाद और अभ्यास से समझ बढ़ती है।"},
        {"समाज", "समाज आपसी सहयोग, समान गरिमा और न्याय से मजबूत बनता है।"}
    };
    return dictionary;
}

inline std::string domainLookup(const std::string& token) {
    const auto key = normalizeKey(token);
    const auto& relations = relationshipDictionary();
    const auto it = relations.find(key);
    if (it != relations.end()) return it->second;
    static const std::unordered_map<std::string, std::string> facts = {
        {"रोटी", "रोटी आटे से बनने वाला सामान्य भोजन है; संतुलित आहार में विविधता रखें।"},
        {"दूध", "दूध पोषक तत्वों का स्रोत है; आहार संबंधी जरूरतें व्यक्ति के अनुसार बदलती हैं।"},
        {"शेर", "शेर एक बड़ा जंगली बिल्ली-प्रजाति का स्तनधारी है।"},
        {"गाय", "गाय पालतू स्तनधारी पशु है; पशुओं के साथ दयालु व्यवहार करें।"},
        {"तोता", "तोता रंगीन पक्षी है; पक्षियों के प्राकृतिक आवास का संरक्षण जरूरी है।"},
        {"खुशी", "खुशी प्रसन्नता का भाव है; भावनाओं को पहचानना और साझा करना सहायक हो सकता है।"}
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
                model_[words[i]][words[i + 1]][words[i + 2]]++;
        }
    }
    std::string generateSeeded(std::mt19937& rng, int maxWords,
                               const std::string& seed) const {
        if (model_.empty()) return "कहानी में पात्र ने धैर्य और सहयोग से समस्या का हल खोजा।";
        auto first = model_.begin();
        std::string w1 = seed, w2 = seed;
        std::string output = seed;
        const auto seedIt = model_.find(seed);
        if (seedIt != model_.end() && !seedIt->second.empty()) {
            std::vector<std::string> next;
            std::vector<double> weights;
            for (const auto& pair : seedIt->second) {
                next.push_back(pair.first);
                std::size_t total = 0;
                for (const auto& word : pair.second) total += static_cast<std::size_t>(word.second);
                weights.push_back(static_cast<double>(total));
            }
            std::discrete_distribution<std::size_t> distribution(weights.begin(), weights.end());
            w2 = next[distribution(rng)];
            output += " " + w2;
        } else {
            while (first != model_.end() && first->second.empty()) ++first;
            if (first == model_.end()) return output;
            const auto second = first->second.begin();
            w1 = first->first;
            w2 = second->first;
            output += " " + w1 + " " + w2;
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
            std::discrete_distribution<std::size_t> distribution(weights.begin(), weights.end());
            const std::string next = choices[distribution(rng)];
            output += " " + next;
            w1 = w2;
            w2 = next;
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
        if (turns_.size() == kMaxTurns) turns_.pop_front();
        turns_.push_back({truncateCodePoints(user, kMaxUserCodePoints),
                          truncateCodePoints(assistant, kMaxAnswerCodePoints)});
    }
private:
    std::deque<ConversationTurn> turns_;
};

class Brain {
public:
    Brain() : stories_(buildNarratives()) {
        model_.train(stories_);
    }
    std::string process(const std::string& raw) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string input = trimText(truncateCodePoints(raw, ConversationMemory::kMaxUserCodePoints));
        if (input.empty()) return "कुछ लिखिए, मैं सुन रहा हूँ।";
        std::string response;
        if (input == "बाहर" || normalizeKey(input) == "exit") {
            response = "अलविदा! Z+2AI से बात करने के लिए धन्यवाद।";
        } else if (const auto answer = kb_.find(input)) {
            response = *answer;
        } else {
            for (const auto& word : tokenizeWords(input)) {
                response = domainLookup(word);
                if (!response.empty()) break;
            }
            if (response.empty()) {
                const std::string seed = extractCoreKeyword(input);
                if (seed.empty()) {
                    response = "Z+2AI आपकी बात समझने का प्रयास कर रहा है। कृपया कोई विषय या प्रश्न लिखें।";
                } else {
                    // Stable per-seed initialization makes fallback output repeatable
                    // for the same topic while retaining weighted trigram selection.
                    std::uint32_t hash = 2166136261u;
                    for (unsigned char byte : seed) { hash ^= byte; hash *= 16777619u; }
                    std::mt19937 localRng(hash);
                    const std::string body = model_.generateSeeded(localRng, 12, seed);
                    response = "Z+2AI विषय: " + seed + "।\n" +
                        "एक छोटी सीख: " + body +
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

inline Brain& brain() {
    static Brain instance;
    return instance;
}
} // namespace z2ai

#ifdef __ANDROID__
extern "C" JNIEXPORT jstring JNICALL
Java_com_zeeshan_ai_MainActivity_nativeGetAIResponse(JNIEnv* env, jobject, jstring input) {
    std::string result;
    try {
        std::u16string text;
        if (input != nullptr) {
            const jsize length = env->GetStringLength(input);
            const jchar* chars = env->GetStringChars(input, nullptr);
            if (chars != nullptr) {
                text.reserve(static_cast<std::size_t>(std::min<jsize>(length, 256)));
                std::size_t points = 0;
                for (jsize i = 0; i < length && points < 256; ++i, ++points) {
                    const char16_t unit = static_cast<char16_t>(chars[i]);
                    text.push_back(unit);
                    if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < length &&
                        chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
                        text.push_back(static_cast<char16_t>(chars[++i]));
                    }
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
