#include "mp4container.h"
#include "mp4tagfield.h"
#include "mp4atom.h"
#include "mp4ids.h"
#include "../exceptions.h"

#include <c++utilities/io/binaryreader.h>
#include <c++utilities/io/binarywriter.h>
#include <c++utilities/misc/memory.h>

#include <algorithm>
#include <memory>
#include <limits>

using namespace std;
using namespace IoUtilities;
using namespace ConversionUtilities;

namespace Media {

/*!
 * \class Media::Mp4TagField
 * \brief The Mp4TagField class is used by Mp4Tag to store the fields.
 */

/*!
 * \brief Constructs a new Mp4TagField.
 */
Mp4TagField::Mp4TagField() :
    m_parsedRawDataType(RawDataType::Reserved),
    m_countryIndicator(0),
    m_langIndicator(0)
{}

/*!
 * \brief Constructs a new Mp4TagField with the specified \a id and \a value.
 */
Mp4TagField::Mp4TagField(identifierType id, const TagValue &value) :
    TagField<Mp4TagField>(id, value),
    m_parsedRawDataType(RawDataType::Reserved),
    m_countryIndicator(0),
    m_langIndicator(0)
{}

/*!
 * \brief Constructs a new Mp4TagField with the specified \a mean, \a name and \a value.
 *
 * The ID will be set to Mp4TagAtomIds::Extended indicating an tag field using the
 * reverse DNS style.
 *
 * \sa The last paragraph of <a href="http://atomicparsley.sourceforge.net/mpeg-4files.html">Known iTunes Metadata Atoms</a>
 *     gives additional information about this form of MP4 tag fields.
 */
Mp4TagField::Mp4TagField(const string &mean, const string &name, const TagValue &value) :
    Mp4TagField(Mp4TagAtomIds::Extended, value)
{
    m_name = name;
    m_mean = mean;
}

/*!
 * \brief Parses field information from the specified Mp4Atom.
 *
 * The specified atom should be a child atom of the "ilst" atom.
 * Each child of the "ilst" atom holds one field of the Mp4Tag.
 *
 * \throws Throws std::ios_base::failure when an IO error occurs.
 * \throws Throws Media::Failure or a derived exception when a parsing
 *         error occurs.
 */
void Mp4TagField::reparse(Mp4Atom &ilstChild)
{
    // prepare reparsing
    using namespace Mp4AtomIds;
    using namespace Mp4TagAtomIds;
    invalidateStatus();
    string context("parsing MP4 tag field");
    clear(); // clear old values
    ilstChild.parse(); // ensure child has been parsed
    setId(ilstChild.id());
    context = "parsing MP4 tag field " + ilstChild.idToString();
    iostream &stream = ilstChild.stream();
    BinaryReader &reader = ilstChild.container().reader();
    int dataAtomFound = 0, meanAtomFound = 0, nameAtomFound = 0;
    for(Mp4Atom *dataAtom = ilstChild.firstChild(); dataAtom; dataAtom = dataAtom->nextSibling()) {
        try {
            dataAtom->parse();
            if(dataAtom->id() == Mp4AtomIds::Data) {
                if(dataAtom->dataSize() < 8) {
                    addNotification(NotificationType::Warning, "Truncated child atom \"data\" in tag atom (ilst child) found. (will be ignored)", context);
                    continue;
                }
                if(++dataAtomFound > 1) {
                    if(dataAtomFound == 2) {
                        addNotification(NotificationType::Warning, "Multiple \"data\" child atom in tag atom (ilst child) found. (will be ignored)", context);
                    }
                    continue;
                }
                stream.seekg(dataAtom->dataOffset());
                if(reader.readByte() != 0) {
                    addNotification(NotificationType::Warning, "The version indicator byte is not zero, the tag atom might be unsupported and hence not be parsed correctly.", context);
                }
                setTypeInfo(m_parsedRawDataType = reader.readUInt24BE());
                try { // try to show warning if parsed raw data type differs from expected raw data type for this atom id
                    vector<uint32> expectedRawDataTypes = this->expectedRawDataTypes();
                    if(find(expectedRawDataTypes.cbegin(), expectedRawDataTypes.cend(), m_parsedRawDataType) == expectedRawDataTypes.cend()) {
                        addNotification(NotificationType::Warning, "Unexpected data type indicator found.", context);
                    }
                } catch(Failure &) {
                    // tag id is unknown, it is not possible to validate parsed data type
                }
                m_countryIndicator = reader.readUInt16BE();
                m_langIndicator = reader.readUInt16BE();
                switch(m_parsedRawDataType) {
                case RawDataType::Utf8: case RawDataType::Utf16:
                    stream.seekg(dataAtom->dataOffset() + 8);
                    value().assignText(reader.readString(dataAtom->dataSize() - 8), (m_parsedRawDataType == RawDataType::Utf16) ? TagTextEncoding::Utf16BigEndian : TagTextEncoding::Utf8);
                    break;
                case RawDataType::Gif: case RawDataType::Jpeg: case RawDataType::Png: case RawDataType::Bmp: {
                    switch(m_parsedRawDataType) {
                    case RawDataType::Gif:
                        value().setMimeType("image/gif");
                        break;
                    case RawDataType::Jpeg:
                        value().setMimeType("image/jpeg");
                        break;
                    case RawDataType::Png:
                        value().setMimeType("image/png");
                        break;
                    case RawDataType::Bmp:
                        value().setMimeType("image/bmp");
                        break;
                    default:
                        ;
                    }
                    streamsize coverSize = dataAtom->dataSize() - 8;
                    unique_ptr<char []> coverData = make_unique<char []>(coverSize);
                    stream.read(coverData.get(), coverSize);
                    value().assignData(move(coverData), coverSize, TagDataType::Picture);
                    break;
                } case RawDataType::BeSignedInt: {
                    int number = 0;
                    if(dataAtom->dataSize() > (8 + 4)) {
                        addNotification(NotificationType::Warning, "Data atom stores integer of invalid size. Trying to read data anyways.", context);
                    }
                    if(dataAtom->dataSize() >= (8 + 4)) {
                        number = reader.readInt32BE();
                    } else if(dataAtom->dataSize() == (8 + 2)) {
                        number = reader.readInt16BE();
                    } else if(dataAtom->dataSize() == (8 + 1)) {
                        number = reader.readChar();
                    }
                    switch(ilstChild.id()) {
                    case PreDefinedGenre: // consider number as standard genre index
                        value().assignStandardGenreIndex(number);
                        break;
                    default:
                        value().assignInteger(number);
                    }
                    break;
                } case RawDataType::BeUnsignedInt: {
                    int number = 0;
                    if(dataAtom->dataSize() > (8 + 4)) {
                        addNotification(NotificationType::Warning, "Data atom stores integer of invalid size. Trying to read data anyways.", context);
                    }
                    if(dataAtom->dataSize() >= (8 + 4)) {
                        number = static_cast<int>(reader.readUInt32BE());
                    } else if(dataAtom->dataSize() == (8 + 2)) {
                        number = static_cast<int>(reader.readUInt16BE());
                    } else if(dataAtom->dataSize() == (8 + 1)) {
                        number = static_cast<int>(reader.readByte());
                    }
                    switch(ilstChild.id()) {
                    case PreDefinedGenre: // consider number as standard genre index
                        value().assignStandardGenreIndex(number - 1);
                        break;
                    default:
                        value().assignInteger(number);
                    }
                    break;
                } default:
                    switch(ilstChild.id()) {
                    // track number, disk number and genre have no specific data type id
                    case TrackPosition:
                    case DiskPosition: {
                        if(dataAtom->dataSize() < (8 + 6)) {
                            addNotification(NotificationType::Warning, "Track/disk position is truncated. Trying to read data anyways.", context);
                        }
                        uint16 pos = 0, total = 0;
                        if(dataAtom->dataSize() >= (8 + 4)) {
                            stream.seekg(2, ios_base::cur);
                            pos = reader.readUInt16BE();
                        }
                        if(dataAtom->dataSize() >= (8 + 6)) {
                            total = reader.readUInt16BE();
                        }
                        value().assignPosition(PositionInSet(pos, total));
                        break;
                    }
                    case PreDefinedGenre:
                        if(dataAtom->dataSize() < (8 + 2)) {
                            addNotification(NotificationType::Warning, "Genre index is truncated.", context);
                        } else {
                            value().assignStandardGenreIndex(reader.readUInt16BE() - 1);
                        }
                        break;
                    default: // no supported data type, read raw data
                        streamsize dataSize = dataAtom->dataSize() - 8;
                        unique_ptr<char []> data = make_unique<char[]>(dataSize);
                        stream.read(data.get(), dataSize);
                        if(ilstChild.id() == Mp4TagAtomIds::Cover) {
                            value().assignData(move(data), dataSize, TagDataType::Picture);
                        } else {
                            value().assignData(move(data), dataSize, TagDataType::Undefined);
                        }
                    }
                }
            } else if(dataAtom->id() == Mp4AtomIds::Mean) {
                if(dataAtom->dataSize() < 8) {
                    addNotification(NotificationType::Warning, "Truncated child atom \"mean\" in tag atom (ilst child) found. (will be ignored)", context);
                    continue;
                }
                if(++meanAtomFound > 1) {
                    if(meanAtomFound == 2) {
                        addNotification(NotificationType::Warning, "Tag atom contains more than one mean atom. The addiational mean atoms will be ignored.", context);
                    }
                    continue;
                }
                stream.seekg(dataAtom->dataOffset() + 4);
                m_mean = reader.readString(dataAtom->dataSize() - 4);
            } else if(dataAtom->id() == Mp4AtomIds::Name) {
                if(dataAtom->dataSize() < 4) {
                    addNotification(NotificationType::Warning, "Truncated child atom \"name\" in tag atom (ilst child) found. (will be ignored)", context);
                    continue;
                }
                if(++nameAtomFound > 1) {
                    if(nameAtomFound == 2) {
                        addNotification(NotificationType::Warning, "Tag atom contains more than one name atom. The addiational name atoms will be ignored.", context);
                    }
                    continue;
                }
                stream.seekg(dataAtom->dataOffset() + 4);
                m_name = reader.readString(dataAtom->dataSize() - 4);
            } else {
                addNotification(NotificationType::Warning, "Unkown child atom \"" + dataAtom->idToString() + "\" in tag atom (ilst child) found. (will be ignored)", context);
            }
        } catch(Failure &) {
            addNotification(NotificationType::Warning, "Unable to parse all childs atom in tag atom (ilst child) found. (will be ignored)", context);
        }
    }
    if(value().isEmpty()) {
        addNotification(NotificationType::Warning, "The field value is empty.", context);
    }
}

/*!
 * \brief Saves the field to the specified \a stream.
 *
 * \throws Throws std::ios_base::failure when an IO error occurs.
 * \throws Throws Media::Failure or a derived exception when a making
 *                error occurs.
 */
void Mp4TagField::make(ostream &stream)
{
    invalidateStatus();
    if(!id()) {
        addNotification(NotificationType::Warning, "Invalid tag atom id.", "making MP4 tag field");
        throw InvalidDataException();
    }
    const string context("making MP4 tag field " + ConversionUtilities::interpretIntegerAsString<identifierType>(id()));
    // there might be only mean and name info, but no data
    if(value().isEmpty() && (!m_mean.empty() || !m_name.empty())) {
        addNotification(NotificationType::Critical, "No tag value assigned.", context);
        throw InvalidDataException();
    }
    uint32 rawDataType = 0;
    try {
        // try to use appropriate raw data type
        rawDataType = appropriateRawDataType();
    } catch(Failure &) {
        // unable to obtain appropriate raw data type
        // assume utf-8 text
        rawDataType = RawDataType::Utf8;
        addNotification(NotificationType::Warning, "It was not possible to find an appropriate raw data type id. UTF-8 will be assumed.", context);
    }
    stringstream convertedData(stringstream::in | stringstream::out | stringstream::binary);
    BinaryWriter writer(&convertedData);
    try {
        if(!value().isEmpty()) { // there might be only mean and name info, but no data
            convertedData.exceptions(std::stringstream::failbit | std::stringstream::badbit);
            switch(rawDataType) {
            case RawDataType::Utf8:
            case RawDataType::Utf16:
                writer.writeString(value().toString());
                break;
            case RawDataType::BeSignedInt: {
                int number = value().toInteger();
                if(number <= numeric_limits<int16>::max()
                        && number >= numeric_limits<int16>::min()) {
                    writer.writeInt16BE(static_cast<int16>(number));
                } else {
                    writer.writeInt32BE(number);
                }
                break;
            } case RawDataType::BeUnsignedInt: {
                int number = value().toInteger();
                if(number <= numeric_limits<uint16>::max()
                        && number >= numeric_limits<uint16>::min()) {
                    writer.writeUInt16BE(static_cast<uint16>(number));
                } else if(number > 0) {
                    writer.writeUInt32BE(number);
                } else {
                    throw ConversionException("Negative integer can not be assigned to the field with the id \"" + interpretIntegerAsString<uint32>(id()) + "\".");
                }
                break;
            } case RawDataType::Bmp: case RawDataType::Jpeg: case RawDataType::Png:
                break; // leave converted data empty to write original data later
            default:
                switch(id()) {
                // track number and disk number are exceptions
                // raw data type 0 is used, information is stored as pair of unsigned integers
                case Mp4TagAtomIds::TrackPosition: case Mp4TagAtomIds::DiskPosition: {
                    PositionInSet pos = value().toPositionIntSet();
                    writer.writeInt32BE(pos.position());
                    if(pos.total() <= numeric_limits<int16>::max()) {
                        writer.writeInt16BE(static_cast<int16>(pos.total()));
                    } else {
                        throw ConversionException("Integer can not be assigned to the field with the id \"" + interpretIntegerAsString<uint32>(id()) + "\" because it is to big.");
                    }
                    writer.writeUInt16BE(0);
                    break;
                }
                case Mp4TagAtomIds::PreDefinedGenre:
                    writer.writeUInt16BE(value().toStandardGenreIndex());
                    break;
                default:
                    ; // leave converted data empty to write original data later
                }
            }
        }
    } catch (ConversionException &ex) {
        // it was not possible to perform required conversions
        if(char_traits<char>::length(ex.what())) {
            addNotification(NotificationType::Critical, ex.what(), context);
        } else {
            addNotification(NotificationType::Critical, "The assigned tag value can not be converted to be written appropriately.", context);
        }
        throw InvalidDataException();
    }
    // data could be converted successfully
    // write data to output stream
    writer.setStream(&stream);
    uint32 dataSize = value().isEmpty()           // calculate data size
            ? 0 : (convertedData.tellp() ? static_cast<size_t>(convertedData.tellp()) : value().dataSize());
    uint32 entireSize = 8                       // calculate entire size
            + (name().empty() ? 0 : (12 + name().length()))
            + (mean().empty() ? 0 : (12 + mean().length()))
            + (dataSize ? (16 + dataSize) : 0);
    writer.writeUInt32BE(entireSize);             // size of entire tag atom
    writer.writeUInt32BE(id());                // id of tag atom
    if(!mean().empty()) {             // write "mean"
        writer.writeUInt32BE(12 + mean().length());
        writer.writeUInt32BE(Mp4AtomIds::Mean);
        writer.writeUInt32BE(0);
        writer.writeString(mean());
    }
    if(!name().empty()) {             // write "name"
        writer.writeUInt32BE(12 + name().length());
        writer.writeUInt32BE(Mp4AtomIds::Name);
        writer.writeUInt32BE(0);
        writer.writeString(name());
    }
    if(!value().isEmpty()) {                      // write data
        writer.writeUInt32BE(16 + dataSize);      // size of data atom
        writer.writeUInt32BE(Mp4AtomIds::Data);   // id of data atom
        writer.writeByte(0);                    // version
        writer.writeUInt24BE(rawDataType);
        writer.writeUInt16BE(m_countryIndicator);
        writer.writeUInt16BE(m_langIndicator);
        if(convertedData.tellp()) {
            stream << convertedData.rdbuf();    // write converted data
        } else { // no conversion was needed, write data directly from tag value
            stream.write(value().dataPointer(), value().dataSize());
        }
    }
}

/*!
 * \brief Returns the expected raw data types for the ID of the field.
 */
std::vector<uint32> Mp4TagField::expectedRawDataTypes() const
{
    using namespace Mp4TagAtomIds;
    std::vector<uint32> res;
    switch(id()) {
    case Album: case Artist: case Comment:
    case Year: case Title: case Genre:
    case Composer: case Encoder: case Grouping:
    case Description: case Lyrics: case RecordLabel:
    case Performers: case Lyricist:
        res.push_back(RawDataType::Utf8);
        res.push_back(RawDataType::Utf16);
        break;
    case PreDefinedGenre: case TrackPosition: case DiskPosition:
        res.push_back(RawDataType::Reserved);
        break;
    case Bpm: case Rating:
        res.push_back(RawDataType::BeSignedInt);
        res.push_back(RawDataType::BeUnsignedInt);
        break;
    case Cover:
        res.push_back(RawDataType::Gif);
        res.push_back(RawDataType::Jpeg);
        res.push_back(RawDataType::Png);
        res.push_back(RawDataType::Bmp);
        break;
    case Extended:
        throw Failure();
    default:
        throw Failure();
    }
    return res;
}

/*!
 * \brief Returns an appropriate raw data type.
 *
 * Returns the type info if assigned; otherwise returns
 * an raw data type considered as appropriate for the ID
 * of the field.
 */
uint32 Mp4TagField::appropriateRawDataType() const
{
    using namespace Mp4TagAtomIds;
    if(isTypeInfoAssigned()) {
        // obtain raw data type from tag field if present
        return typeInfo();
    } else {
        // there is no raw data type assigned (tag field was not
        // present in original file but rather was added manually)
        // try to derive appropriate raw data type from atom id
        switch(id()) {
        case Album: case Artist: case Comment:
        case Year: case Title: case Genre:
        case Composer: case Encoder: case Grouping:
        case Description: case Lyrics: case RecordLabel:
        case Performers: case Lyricist:
            switch(value().dataEncoding()) {
            case TagTextEncoding::Utf8: return RawDataType::Utf8;
            case TagTextEncoding::Utf16BigEndian: return RawDataType::Utf16;
            default: throw Failure();
            }
        case TrackPosition: case DiskPosition:
            return RawDataType::Reserved;
        case PreDefinedGenre: case Bpm: case Rating:
            return RawDataType::BeSignedInt;
        case Cover: {
            const string &mimeType = value().mimeType();
            if(mimeType == "image/jpg" || mimeType == "image/jpeg") { // "well-known" type
                return RawDataType::Jpeg;
            } else if(mimeType == "image/png") {
                return RawDataType::Png;
            } else if(mimeType == "image/bmp") {
                return RawDataType::Bmp;
            } else {
                throw Failure();
            }
        }
        case Extended:
            throw Failure();
        default:
            throw Failure();
        }
    }
}

/*!
 * \brief Ensures the field is cleared.
 */
void Mp4TagField::cleared()
{
    m_name.clear();
    m_mean.clear();
    m_parsedRawDataType = RawDataType::Reserved;
    m_countryIndicator = 0;
    m_langIndicator = 0;
}

}