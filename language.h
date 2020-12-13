#ifndef TAG_PARSER_LANGUAGE_H
#define TAG_PARSER_LANGUAGE_H

#include "./global.h"

#include <c++utilities/conversion/stringbuilder.h>

#include <cstdint>
#include <string>
#include <optional>

namespace TagParser {

/*!
 * \brief Returns whether an ISO-639-2 \a languageSpecification is not empty or undefined.
 */
inline bool isLanguageDefined(const std::string &languageSpecification)
{
    return !languageSpecification.empty() && languageSpecification != "und";
}

TAG_PARSER_EXPORT const std::string &languageNameFromIso(const std::string &isoCode);
TAG_PARSER_EXPORT const std::string &languageNameFromIsoWithFallback(const std::string &isoCode);

/// \brief The LocaleFormat enum class specifies the format used by a LocaleDetail.
enum class LocaleDetailFormat : std::uint64_t {
    Unknown, /*! the format is unknown */
    ISO_639_1, /*! a language specified via ISO-639-1 code (e.g. "de" for German) */
    ISO_639_2_T, /*! a language specified via ISO-639-2/T code (terminological, e.g. "deu" for German) */
    ISO_639_2_B, /*! a language specified via ISO-639-2/B code (bibliographic, e.g. "ger" for German) */
    DomainCountry, /*! a country as used by [Internet domains](https://www.iana.org/domains/root/db) (e.g. "de" for Germany or "at" for Austria) */
    BCP_47, /*! a language and/or country according to [BCP 47](https://tools.ietf.org/html/bcp47) using
                the [IANA Language Subtag Registry](https://www.iana.com/assignments/language-subtag-registry/language-subtag-registry)
                (e.g. "de_DE" for the language/country German/Germany or "de_AT" for German/Austria) */
};

/// \brief The LocaleInfo struct specifies a language and/or country.
struct TAG_PARSER_EXPORT LocaleDetail {
    std::string value;
    LocaleDetailFormat format;
};

/// \brief The Locale struct contains a number of LocaleDetail structs which make up a locale information.
struct TAG_PARSER_EXPORT Locale : public std::vector<LocaleDetail> {
    /// \brief Returns a display name of the locale, e.g. Germany.
    const std::string &displayName() const;
    /// \brief Returns whether the local is valid if that can be determined.
    std::optional<bool> isValid() const;
};

} // namespace TagParser

#endif // TAG_PARSER_LANGUAGE_H
