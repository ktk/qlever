//
// Created by johannes on 18.12.19.
//

#ifndef QLEVER_STRINGSORTCOMPARATOR_H
#define QLEVER_STRINGSORTCOMPARATOR_H

#include <unicode/casemap.h>
#include <unicode/coll.h>
#include <unicode/locid.h>
#include <unicode/unistr.h>
#include <unicode/utypes.h>
#include <unicode/normalizer2.h>
#include <unicode/unorm2.h>
#include <cstring>
#include <memory>
#include "../global/Constants.h"
#include "../util/Exception.h"
#include "../util/StringUtils.h"

/**
 * @brief This class wraps all calls to the ICU library that are required by
 * QLever It internally handles all conversion to and from UTF-8 and from c++ to
 * c-strings where they are required by ICU
 */
class LocaleManager {
 public:
  /// The five collation levels supported by icu, forwarded in a typesafe manner
  enum class Level : uint8_t {
    PRIMARY = 0,
    SECONDARY = 1,
    TERTIARY = 2,
    QUARTERNARY = 3,
    IDENTICAL = 4
  };



  /**
   * Wraps a string that contains unicode collation weights for another string
   * Only needed for making interfaces explicit and less errorProne
   */
  class SortKey {
   public:
    SortKey() = default;
    explicit SortKey(std::string_view contents) : _content(contents) {}
    [[nodiscard]] const std::string& get() const { return _content; }
    std::string& get() { return _content; }

   private:
    std::string _content;
  };

  /// Copy constructor
  LocaleManager(const LocaleManager& rhs)
      : _icuLocale(rhs._icuLocale),
        _ignorePunctuationStatus(rhs._ignorePunctuationStatus) {
    setupCollators();
    setIgnorePunctuationOnFirstLevels(_ignorePunctuationStatus);
  }

  /// Default constructor. Use the settings from "../global/Constants.h"
  LocaleManager()
      : LocaleManager(LOCALE_DEFAULT_LANG, LOCALE_DEFAULT_COUNTRY,
                      LOCALE_DEFAULT_IGNORE_PUNCTUATION){};

  /**
   * @param lang The language of the locale, e.g. "en" or "de"
   * @param country The country of the locale, e.g. "US" or "CA"
   * @param ignorePunctuationAtFirstLevel If true then spaces/punctuation etc.
   * will only be considered for comparisons if strings match otherwise Throws
   * std::runtime_error if the locale cannot be constructed from lang and
   * country args
   *
   * \todo(joka921): make the exact punctuation level configurable.
   */
  LocaleManager(const std::string& lang, const std::string& country,
                bool ignorePunctuationAtFirstLevel) {
    _icuLocale = icu::Locale(lang.c_str(), country.c_str());
    _ignorePunctuationStatus =
        ignorePunctuationAtFirstLevel ? UCOL_SHIFTED : UCOL_NON_IGNORABLE;

    if (_icuLocale.isBogus() == TRUE) {
      throw std::runtime_error("Could not create locale with language " + lang +
                               " and Country " + country);
    }
    setupCollators();
    setIgnorePunctuationOnFirstLevels(_ignorePunctuationStatus);
  }

  /// Assign from another LocaleManager
  LocaleManager& operator=(const LocaleManager& other) {
    if (this == &other) return *this;
    _icuLocale = other._icuLocale;
    _ignorePunctuationStatus = other._ignorePunctuationStatus;
    setupCollators();
    setIgnorePunctuationOnFirstLevels(_ignorePunctuationStatus);
    return *this;
  }

  /**
   * @brief Compare two UTF-8 encoded string_views according to the held Locale
   * @param a
   * @param b
   * @param level Compare according to this collation Level
   * @return <0 iff a<b , >0 iff a>b,  0 iff a==b
   */
  [[nodiscard]] int compare(std::string_view a, std::string_view b,
                            const Level level) const {
    UErrorCode err = U_ZERO_ERROR;
    auto idx = static_cast<uint8_t>(level);
    auto res = compToInd(
        _collator[idx]->compareUTF8(toStringPiece(a), toStringPiece(b), err));
    raise(err);
    return res;
  }

  /**
   * @brief Compare two WeightStrings. These have to be extracted by a call to
   * getSortKey using the same level specification and on the same LocaleManager
   * otherwise the behavior is undefined
   * @param a
   * @param b
   * @param level This parameter is ignored but required to have a symmetric
   * interface
   * @return <0 iff a<b , >0 iff a>b,  0 iff a==b
   */
  static int compare(SortKey a, SortKey b, [[maybe_unused]] const Level) {
    return std::strcmp(a.get().c_str(), b.get().c_str());
  }

  /**
   * @brief Transform a UTF-8 string into a SortKey that can be compared using
   * std::strcmp.
   *
   * We need this wrapper because ICU internally only works on utf16 and does
   * not create c++ strings in large parts of the API
   * @param s A UTF-8 encoded string.
   * @param level The Collation Level for which we want to create the SortKey
   * @return A weight string s.t. compare(s, t, level) ==
   * std::strcmp(getSortKey(s, level), getSortKey(t, level)
   */
  [[nodiscard]] SortKey getSortKey(std::string_view s,
                                   const Level level) const {
    auto utf16 = icu::UnicodeString::fromUTF8(toStringPiece(s));
    auto& col = *_collator[static_cast<uint8_t>(level)];
    auto sz = col.getSortKey(utf16, nullptr, 0);
    SortKey finalRes;
    std::string& res = finalRes.get();
    res.resize(sz);
    static_assert(sizeof(uint8_t) == sizeof(std::string::value_type));
    sz = col.getSortKey(utf16, reinterpret_cast<uint8_t*>(res.data()),
                        res.size());
    AD_CHECK(sz == static_cast<decltype(sz)>(
                       res.size()));  // this is save by the way we obtained sz
    // since this is a c-api we still have a trailing '\0'. Trimming this is
    // necessary for the prefix range to work correct.
    res.resize(res.size() - 1);
    return finalRes;
  }

  /**
   * @brief convert a UTF-8 String to lowercase according to the held locale
   * @param s UTF-8 encoded string
   * @return The lowercase version of s, also encoded as UTF-8
   */
  [[nodiscard]] std::string getLowercaseUtf8(const std::string& s) const {
    std::string res;
    icu::StringByteSink<std::string> sink(&res);
    UErrorCode err = U_ZERO_ERROR;
    icu::CaseMap::utf8ToLower(_icuLocale.getName(), 0, s, sink, nullptr, err);
    raise(err);
    return res;
  }

  /// get a prefix of length prefixLength of the UTF8 string (or shorter, if the
  /// string contains less codepoints This counts utf-8 codepoints correctly and
  /// returns a UTF-8 string referring to the Prefix and the number of Unicode
  /// codepoints it actually encodes ( <= prefixLength).
  /**
   * @brief get a prefix of a utf-8 string of a specified length
   *
   * This will first max(prefixLength, numCodepointsInInput) Codepoints encoded
   * in the sp argument. CAVEAT: This is in most cases wrong when answering the
   * question "is X a prefix of Y" because collation might ignore punctuation
   * etc. This is currently only used for the Text Index where all words that
   * share a common prefix of a certain length are stored in the same block.
   * @param sp a UTF-8 encoded string
   * @param prefixLength The number of Unicode codepoints we want to extract.
   * @return the first max(prefixLength, numCodepointsInArgSP) Unicode
   * codepoints of sp, encoded as UTF-8
   */
  static std::pair<size_t, std::string> getUTF8Prefix(std::string_view sp,
                                                      size_t prefixLength) {
    const char* s = sp.data();
    int32_t length = sp.length();
    size_t numCodepoints = 0;
    int32_t i = 0;
    for (i = 0; i < length && numCodepoints < prefixLength;) {
      UChar32 c;
      U8_NEXT(s, i, length, c);
      if (c >= 0) {
        ++numCodepoints;
      } else {
        throw std::runtime_error(
            "Illegal UTF sequence in LocaleManager::getUTF8Prefix");
      }
    }
    return {numCodepoints, std::string(sp.data(), i)};
  }

  /**
   * @brief Normalize a Utf8 string to a canonical representation.
   * Maps e.g. single codepoint é and e + accent aigu to single codepoint é by applying the UNICODE NFC
   * (Normalization form C)
   * This is independent from the locale
   * @param input The String to be normalized. Must be UTF-8 encoded
   * @return The NFC canonical form of NFC in UTF-8 encoding.
   */
  std::string normalizeUtf8(std::string_view input) const {
    std::string res;
    icu::StringByteSink<std::string> sink(&res);
    UErrorCode err = U_ZERO_ERROR;
    _normalizer->normalizeUTF8(0, toStringPiece(input), sink, nullptr,err);
    raise(err);
    return res;
  }

 private:
  icu::Locale _icuLocale;  // the held locale
  /* One collator for each collation Level to make this class threadsafe.
   * Needed because setting the collation level and comparing strings are 2
   * different steps in icu. */
  std::unique_ptr<icu::Collator> _collator[5];
  UColAttributeValue _ignorePunctuationStatus =
      UCOL_NON_IGNORABLE;  // how to sort punctuations etc.

  const icu::Normalizer2* _normalizer; // actually locale-independent but useful to be placed here since it wraps ICU

  // raise an exception if the error code holds an error.
  static void raise(const UErrorCode& err) {
    if (U_FAILURE(err)) {
      throw std::runtime_error(u_errorName(err));
    }
  }

  /* create one collator for each of the possible collation levels.
   * has to be called each time the locale is changed. */
  void setupCollators() {
    for (auto& col : _collator) {
      UErrorCode err = U_ZERO_ERROR;
      col.reset(icu::Collator::createInstance(_icuLocale, err));
      raise(err);
    }
    _collator[static_cast<uint8_t>(Level::PRIMARY)]->setStrength(
        icu::Collator::PRIMARY);
    _collator[static_cast<uint8_t>(Level::SECONDARY)]->setStrength(
        icu::Collator::SECONDARY);
    _collator[static_cast<uint8_t>(Level::TERTIARY)]->setStrength(
        icu::Collator::TERTIARY);
    _collator[static_cast<uint8_t>(Level::QUARTERNARY)]->setStrength(
        icu::Collator::QUATERNARY);
    _collator[static_cast<uint8_t>(Level::IDENTICAL)]->setStrength(
        icu::Collator::IDENTICAL);

    // also setup the normalizer
    UErrorCode err = U_ZERO_ERROR;
    _normalizer = icu::Normalizer2::getInstance(nullptr, "nfc", UNORM2_COMPOSE, err);
    raise(err);
  }

  // ______________________________________________________________________________
  void setIgnorePunctuationOnFirstLevels(UColAttributeValue val) {
    _ignorePunctuationStatus = val;
    UErrorCode err = U_ZERO_ERROR;
    for (auto& col : _collator) {
      col->setAttribute(UCOL_ALTERNATE_HANDLING, val, err);
      raise(err);
      // todo<joka921> : make this customizable for future versions
      col->setMaxVariable(UCOL_REORDER_CODE_SYMBOL, err);
      raise(err);
    }
  }

  // convert LESS EQUAL GREATER from icu to -1, 0, +1 to make results compatible
  // to std::strcmp
  static int compToInd(const UCollationResult res) {
    switch (res) {
      case UCOL_LESS:
        return -1;
      case UCOL_EQUAL:
        return 0;
      case UCOL_GREATER:
        return 1;
    }
    throw std::runtime_error(
        "Illegal value for UCollationResult. This should never happen!");
  }

  /* This conversion is needed for "older" versions of ICU, e.g. ICU60 which is
   * contained in Ubuntu's LTS repositories */
  static icu::StringPiece toStringPiece(std::string_view s) {
    return icu::StringPiece(s.data(), s.size());
  }
};

/**
 * @brief This class compares strings according to proper Unicode collation,
 * e.g. Strings from the text index vocabulary To Compare components of RDFS
 * triples use the TripleComponentComparator defined below
 */
class SimpleStringComparator {
 public:
  using Level = LocaleManager::Level;
  /**
   * @param lang The language of the locale, e.g. "en" or "de"
   * @param country The country of the locale, e.g. "US" or "CA"
   * @param ignorePunctuationAtFirstLevel If true then spaces/punctuation etc.
   * will only be considered for comparisons if strings match otherwise Throws
   * std::runtime_error if the locale cannot be constructed from lang and
   * country args
   *
   * \todo(joka921): make the exact punctuation level configurable.
   */
  SimpleStringComparator(const std::string& lang, const std::string& country,
                         bool ignorePunctuationAtFirstLevel)
      : _locManager(lang, country, ignorePunctuationAtFirstLevel) {}

  /// Construct according to the default locale specified in
  /// ../global/Constants.h
  SimpleStringComparator() = default;

  /**
   * @brief Compare two UTF-8 encoded strings
   * @return True iff a comes before b
   */
  bool operator()(std::string_view a, std::string_view b,
                  const Level level = Level::QUARTERNARY) const {
    return _locManager.compare(a, b, level) < 0;
  }

  /**
   * @brief Compare a UTF-8 encoded string and a SortKey on the Primary Level
   * CAVEAT: The Level l argument IS IGNORED
   *
   * Since this class only exports WeightStrings on the PRIMARY level via the
   * transformToFirstPossibleBiggerValue method, we also always use the PRIMARY
   * level for this function to avoid mistakes
   * The Level argument is therefore ignored but left in as a dummy to make the
   * getLowerBoundLambda api of the Vocabulary easier.
   * @TODO<joka921> Allow prefix ranges on different levels.
   * @param a A UTF-8 encoded string
   * @param b This Weight string has to be obtained by a previous call to
   * transformToFirstPossibleBiggerValue
   * @
   * @return true iff a comes before the string whose SortKey is b
   */
  bool operator()(std::string_view a, const LocaleManager::SortKey& b,
                  [[maybe_unused]] const Level l) const {
    auto aTrans = _locManager.getSortKey(a, Level::PRIMARY);
    auto cmp = LocaleManager::compare(aTrans, b, Level::PRIMARY);
    return cmp < 0;
  }

  /**
   * @brief Transform a string s to the SortKey of the first possible
   * string that compares greater to s according to the held locale on the
   * PRIMARY level (other levels will cause an assertion fail.
   *
   * This is needed for calculating whether one string is a prefix of another
   * CAVEAT: This currently only supports the primary collation Level!!!
   * <TODO<joka921>: Implement this on every level, either by fixing ICU or by
   * hacking the collation strings
   *
   * @param s A UTF-8 encoded string
   * @return the PRIMARY level SortKey of the first possible string greater than
   * s
   */
  [[nodiscard]] LocaleManager::SortKey transformToFirstPossibleBiggerValue(
      std::string_view s, const Level level) const {
    AD_CHECK(level == Level::PRIMARY);
    auto transformed = _locManager.getSortKey(s, Level::PRIMARY);
    unsigned char last = transformed.get().back();
    if (last < std::numeric_limits<unsigned char>::max()) {
      transformed.get().back() += 1;
    } else {
      transformed.get().push_back('\0');
    }
    return transformed;
  }

  /// Obtain access to the held LocaleManager
  [[nodiscard]] const LocaleManager& getLocaleManager() const {
    return _locManager;
  }

 private:
  LocaleManager _locManager;
};

/**
 * @brief Handles the comparisons between RDFS triple elements according to
 * their data types and proper Unicode collation.
 *
 *  General Approach: First Sort by the datatype, then by the actual value and
 * then by the language tag.
 */
class TripleComponentComparator {
 public:
  using Level = LocaleManager::Level;

  /**
   * @param lang The language of the locale, e.g. "en" or "de"
   * @param country The country of the locale, e.g. "US" or "CA"
   * @param ignorePunctuationAtFirstLevel If true then spaces/punctuation etc.
   * will only be considered for comparisons if strings match otherwise Throws
   * std::runtime_error if the locale cannot be constructed from lang and
   * country args
   *
   * \todo(joka921): make the exact punctuation level configurable.
   */
  TripleComponentComparator(const std::string& lang, const std::string& country,
                            bool ignorePunctuationAtFirstLevel)
      : _locManager(lang, country, ignorePunctuationAtFirstLevel) {}

  /// Construct according to the default locale in "../global/Constants.h"
  TripleComponentComparator() = default;

  /**
   * @brief An entry of the Vocabulary, split up into its components and
   * possibly converted to a format that is easier to compare
   *
   * @tparam ST either LocaleManager::SortKey or std::string_view. Since both
   * variants differ greatly in their usage they are commented with the template
   * instantiations
   */
  template <class InnerString, class LanguageTag>
  struct SplitValBase {
    SplitValBase() = default;
    SplitValBase(char fst, InnerString trans, LanguageTag l)
        : firstOriginalChar(fst),
          transformedVal(std::move(trans)),
          langtag(std::move(l)) {}

    /// The first char of the original value, used to distinguish between
    /// different datatypes
    char firstOriginalChar = '\0';
    InnerString transformedVal;  /// The original inner value, possibly
                                 /// transformed by a locale().
    LanguageTag langtag;         /// the language tag, possibly empty
  };

  /**
   * This value owns all its contents.
   * The inner value is the SortKey of the original inner value according to the
   * held Locale. This is used to transform the inner value and to safely pass
   * it around, e.g. when performing prefix comparisons in the vocabulary
   */
  using SplitVal = SplitValBase<LocaleManager::SortKey, std::string>;

  /**
   * This only holds string_views to substrings of a string.
   * Currently we only use this inside this class
   */
  using SplitValNonOwning = SplitValBase<std::string_view, std::string_view>;

  /**
   * \brief Compare two elements from the Vocabulary.
   * @return false iff a comes before b in the vocabulary
   */
  bool operator()(std::string_view a, std::string_view b,
                  const Level level = Level::QUARTERNARY) const {
    return compare(a, b, level) < 0;
  }

  /**
   * @brief Compare a string_view from the vocabulary to a SplitVal that was
   * previously transformed
   * @param a Element of the vocabulary
   * @param spB this splitVal must have been obtained by a call to
   * extractAndTransformComparable
   * @param level
   * @return a comes before the original value of spB in the vocabulary
   */
  bool operator()(std::string_view a, const SplitVal& spB,
                  const Level level) const {
    auto spA = extractAndTransformComparable(a, level);
    return compare(spA, spB, level) < 0;
  }

  bool operator()(const SplitVal& a, const SplitVal& b,
                  const Level level) const {
    return compare(a, b, level) < 0;
  }

  /// Compare two string_views from the Vocabulary. Return value according to
  /// std::strcmp
  [[nodiscard]] int compare(std::string_view a, std::string_view b,
                            const Level level = Level::QUARTERNARY) const {
    auto splitA = extractComparable<SplitValNonOwning>(a, level);
    auto splitB = extractComparable<SplitValNonOwning>(b, level);
    return compare(splitA, splitB, level);
  }

  /**
   * @brief Split a literal or iri into its components and convert the inner
   * value according to the held locale
   */
  [[nodiscard]] SplitVal extractAndTransformComparable(
      std::string_view a, const Level level) const {
    return extractComparable<SplitVal>(a, level);
  }

  /**
   * @brief the inner comparison logic
   *
   * First compares the datatypes by the firstOriginalChar, then the inner value
   * and then the language tags
   * @return <0 iff a<b, 0 iff a==b, >0 iff a>b
   */
  template <class A, class B>
  [[nodiscard]] int compare(const SplitValBase<A, B>& a,
                            const SplitValBase<A, B>& b,
                            const Level level) const {
    if (auto res = std::strncmp(&a.firstOriginalChar, &b.firstOriginalChar, 1);
        res != 0) {
      return res;  // different data types, decide on the datatype
    }

    if (int res =
            // this correctly dispatches between SortKeys (already transformed)
            // and string_views (not-transformed, perform unicode collation)
        _locManager.compare(a.transformedVal, b.transformedVal, level);
        res != 0) {
      return res;  // actual value differs
    }
    return a.langtag.compare(
        b.langtag);  // if everything else matches, we sort by the langtag
  }

  /**
   *
   * @brief Transform a string s from the vocabulary to the SplitVal of the
   * first possible vocabulary string that compares greater to s according to
   * the held locale on the PRIMARY level (other levels will cause an assertion
   * fail.)
   *
   * This is needed for calculating whether one string is a prefix of another
   * CAVEAT: This currently only supports the primary collation Level!!!
   * <TODO<joka921>: Implement this on every level, either by fixing ICU or by
   * hacking the collation strings
   *
   * @param s A UTF-8 encoded string that contains an element of an RDF triple
   * @param level must be Level::PRIMARY
   * @return the PRIMARY level SortKey of the first possible string greater than
   * s
   */
  [[nodiscard]] SplitVal transformToFirstPossibleBiggerValue(
      std::string_view s, const Level level) const {
    AD_CHECK(level == Level::PRIMARY);
    auto transformed = extractAndTransformComparable(s, Level::PRIMARY);
    unsigned char last = transformed.transformedVal.get().back();
    if (last < std::numeric_limits<unsigned char>::max()) {
      transformed.transformedVal.get().back() += 1;
    } else {
      transformed.transformedVal.get().push_back('\0');
    }
    return transformed;
  }

  /// obtain const access to the held LocaleManager
  [[nodiscard]] const LocaleManager& getLocaleManager() const {
    return _locManager;
  }

  /**
   * @brief Normalize a Utf8 string to a canonical representation.
   * Maps e.g. single codepoint é and e + accent aigu to single codepoint é by applying the UNICODE NFC
   * (Normalization form C)
   * This is independent from the locale
   * @param input The String to be normalized. Must be UTF-8 encoded
   * @return The NFC canonical form of NFC in UTF-8 encoding.
   */
  std::string normalizeUtf8(std::string_view sv) const {
    return _locManager.normalizeUtf8(sv);
  }

 private:
  LocaleManager _locManager;

  /* Split a string into its components to prepare collation.
   * SplitValType = SplitVal will transform the inner string according to the
   * locale SplitValTye = SplitValNonOwning will leave the inner string as is.
   */
  template <class SplitValType>
  [[nodiscard]] SplitValType extractComparable(
      std::string_view a, [[maybe_unused]] const Level level) const {
    std::string_view res = a;
    const char first = a.empty() ? char(0) : a[0];
    std::string_view langtag;
    if (ad_utility::startsWith(res, "\"")) {
      // only remove the first character in case of literals that always start
      // with a quotation mark. For all other types we need this. <TODO> rework
      // the vocabulary's data type to remove ALL of those hacks
      res.remove_prefix(1);
      // In the case of prefix filters we might also have
      // Literals that do not have the closing quotation mark
      auto endPos = ad_utility::findLiteralEnd(res, "\"");
      if (endPos != string::npos) {
        // this should also be fine if there is no langtag (endPos == size()
        // according to cppreference.com
        langtag = res.substr(endPos + 1);
        res.remove_suffix(res.size() - endPos);
      } else {
        langtag = "";
      }
    }
    if constexpr (std::is_same_v<SplitValType, SplitVal>) {
      return {first, _locManager.getSortKey(res, level), std::string(langtag)};
    } else if constexpr (std::is_same_v<SplitValType, SplitValNonOwning>) {
      return {first, res, langtag};
    } else {
      SplitValType().ThisShouldNotCompile();
    }
  }
};

#endif  // QLEVER_STRINGSORTCOMPARATOR_H
