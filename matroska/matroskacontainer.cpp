#include "./matroskacontainer.h"
#include "./ebmlid.h"
#include "./matroskaid.h"
#include "./matroskacues.h"
#include "./matroskaeditionentry.h"
#include "./matroskaseekinfo.h"

#include "../mediafileinfo.h"
#include "../exceptions.h"
#include "../backuphelper.h"

#include "resources/config.h"

#include <c++utilities/conversion/stringconversion.h>
#include <c++utilities/io/catchiofailure.h>
#include <c++utilities/misc/memory.h>

#include <unistd.h>

#include <functional>
#include <initializer_list>
#include <unordered_set>

using namespace std;
using namespace std::placeholders;
using namespace IoUtilities;
using namespace ConversionUtilities;
using namespace ChronoUtilities;

namespace Media {

constexpr const char appInfo[] = APP_NAME " v" APP_VERSION;
constexpr uint64 appInfoElementDataSize = sizeof(appInfo) - 1;
constexpr uint64 appInfoElementTotalSize = 2 + 1 + appInfoElementDataSize;

/*!
 * \class Media::MatroskaContainer
 * \brief Implementation of GenericContainer<MediaFileInfo, MatroskaTag, MatroskaTrack, EbmlElement>.
 */

uint64 MatroskaContainer::m_maxFullParseSize = 0x3200000;

/*!
 * \brief Constructs a new container for the specified \a fileInfo at the specified \a startOffset.
 */
MatroskaContainer::MatroskaContainer(MediaFileInfo &fileInfo, uint64 startOffset) :
    GenericContainer<MediaFileInfo, MatroskaTag, MatroskaTrack, EbmlElement>(fileInfo, startOffset),
    m_maxIdLength(4),
    m_maxSizeLength(8),
    m_segmentCount(0)
{
    m_version = 1;
    m_readVersion = 1;
    m_doctype = "matroska";
    m_doctypeVersion = 1;
    m_doctypeReadVersion = 1;
}

MatroskaContainer::~MatroskaContainer()
{}

void MatroskaContainer::reset()
{
    GenericContainer<MediaFileInfo, MatroskaTag, MatroskaTrack, EbmlElement>::reset();
    m_maxIdLength = 4;
    m_maxSizeLength = 8;
    m_version = 1;
    m_readVersion = 1;
    m_doctype = "matroska";
    m_doctypeVersion = 1;
    m_doctypeReadVersion = 1;
    m_tracksElements.clear();
    m_segmentInfoElements.clear();
    m_tagsElements.clear();
    m_chaptersElements.clear();
    m_attachmentsElements.clear();
    m_seekInfos.clear();
    m_editionEntries.clear();
    m_attachments.clear();
    m_segmentCount = 0;
}

/*!
 * \brief Validates the file index (cue entries).
 * \remarks Checks only for cluster positions and missing, unknown or surplus elements.
 */
void MatroskaContainer::validateIndex()
{
    static const string context("validating Matroska file index (cues)");
    bool cuesElementsFound = false;
    if(m_firstElement) {
        unordered_set<int> ids;
        bool cueTimeFound = false, cueTrackPositionsFound = false;
        unique_ptr<EbmlElement> clusterElement;
        uint64 pos, prevClusterSize = 0, currentOffset = 0;
        // iterate throught all segments
        for(EbmlElement *segmentElement = m_firstElement->siblingById(MatroskaIds::Segment); segmentElement; segmentElement = segmentElement->siblingById(MatroskaIds::Segment)) {
            segmentElement->parse();
            // iterate throught all child elements of the segment (only "Cues"- and "Cluster"-elements are relevant for this method)
            for(EbmlElement *segmentChildElement = segmentElement->firstChild(); segmentChildElement; segmentChildElement = segmentChildElement->nextSibling()) {
                segmentChildElement->parse();
                switch(segmentChildElement->id()) {
                case EbmlIds::Void:
                case EbmlIds::Crc32:
                    break;
                case MatroskaIds::Cues:
                    cuesElementsFound = true;
                    // parse childs of "Cues"-element ("CuePoint"-elements)
                    for(EbmlElement *cuePointElement = segmentChildElement->firstChild(); cuePointElement; cuePointElement = cuePointElement->nextSibling()) {
                        cuePointElement->parse();
                        cueTimeFound = cueTrackPositionsFound = false; // to validate quantity of these elements
                        switch(cuePointElement->id()) {
                        case EbmlIds::Void:
                        case EbmlIds::Crc32:
                            break;
                        case MatroskaIds::CuePoint:
                            // parse childs of "CuePoint"-element
                            for(EbmlElement *cuePointChildElement = cuePointElement->firstChild(); cuePointChildElement; cuePointChildElement = cuePointChildElement->nextSibling()) {
                                cuePointChildElement->parse();
                                switch(cuePointChildElement->id()) {
                                case MatroskaIds::CueTime:
                                    // validate uniqueness
                                    if(cueTimeFound) {
                                        addNotification(NotificationType::Warning, "\"CuePoint\"-element contains multiple \"CueTime\" elements.", context);
                                    } else {
                                        cueTimeFound = true;
                                    }
                                    break;
                                case MatroskaIds::CueTrackPositions:
                                    cueTrackPositionsFound = true;
                                    ids.clear();
                                    clusterElement.reset();
                                    for(EbmlElement *subElement = cuePointChildElement->firstChild(); subElement; subElement = subElement->nextSibling()) {
                                        subElement->parse();
                                        switch(subElement->id()) {
                                        case MatroskaIds::CueTrack:
                                        case MatroskaIds::CueClusterPosition:
                                        case MatroskaIds::CueRelativePosition:
                                        case MatroskaIds::CueDuration:
                                        case MatroskaIds::CueBlockNumber:
                                        case MatroskaIds::CueCodecState:
                                            // validate uniqueness
                                            if(ids.count(subElement->id())) {
                                                addNotification(NotificationType::Warning, "\"CueTrackPositions\"-element contains multiple \"" + subElement->idToString() + "\" elements.", context);
                                            } else {
                                                ids.insert(subElement->id());
                                            }
                                            break;
                                        case EbmlIds::Crc32:
                                        case EbmlIds::Void:
                                        case MatroskaIds::CueReference:
                                            break;
                                        default:
                                            addNotification(NotificationType::Warning, "\"CueTrackPositions\"-element contains unknown element \"" + subElement->idToString() + "\".", context);
                                        }
                                        switch(subElement->id()) {
                                        case EbmlIds::Void:
                                        case EbmlIds::Crc32:
                                        case MatroskaIds::CueTrack:
                                            break;
                                        case MatroskaIds::CueClusterPosition:
                                            // validate "Cluster" position denoted by "CueClusterPosition"-element
                                            clusterElement = make_unique<EbmlElement>(*this, segmentElement->dataOffset() + subElement->readUInteger() - currentOffset);
                                            try {
                                            clusterElement->parse();
                                            if(clusterElement->id() != MatroskaIds::Cluster) {
                                                addNotification(NotificationType::Critical, "\"CueClusterPosition\" element at " + numberToString(subElement->startOffset()) + " does not point to \"Cluster\"-element (points to " + numberToString(clusterElement->startOffset()) + ").", context);
                                            }
                                        } catch(const Failure &) {
                                                addNotifications(context, *clusterElement);
                                            }
                                            break;
                                        case MatroskaIds::CueRelativePosition:
                                            // read "Block" position denoted by "CueRelativePosition"-element (validate later since the "Cluster"-element is needed to validate)
                                            pos = subElement->readUInteger();
                                            break;
                                        case MatroskaIds::CueDuration:
                                            break;
                                        case MatroskaIds::CueBlockNumber:
                                            break;
                                        case MatroskaIds::CueCodecState:
                                            break;
                                        case MatroskaIds::CueReference:
                                            break;
                                        default:
                                            ;
                                        }
                                    }
                                    // validate existence of mandatory elements
                                    if(!ids.count(MatroskaIds::CueTrack)) {
                                        addNotification(NotificationType::Warning, "\"CueTrackPositions\"-element does not contain mandatory element \"CueTrack\".", context);
                                    }
                                    if(!clusterElement) {
                                        addNotification(NotificationType::Warning, "\"CueTrackPositions\"-element does not contain mandatory element \"CueClusterPosition\".", context);
                                    } else {
                                        if(ids.count(MatroskaIds::CueRelativePosition)) {
                                            // validate "Block" position denoted by "CueRelativePosition"-element
                                            EbmlElement referenceElement(*this, clusterElement->dataOffset() + pos);
                                            try {
                                                referenceElement.parse();
                                                switch(referenceElement.id()) {
                                                case MatroskaIds::SimpleBlock:
                                                case MatroskaIds::Block:
                                                case MatroskaIds::BlockGroup:
                                                    break;
                                                default:
                                                    addNotification(NotificationType::Critical, "\"CueRelativePosition\" element does not point to \"Block\"-, \"BlockGroup\", or \"SimpleBlock\"-element (points to " + numberToString(referenceElement.startOffset()) + ").", context);
                                                }
                                            } catch(const Failure &) {
                                                addNotifications(context, referenceElement);
                                            }
                                        }
                                    }
                                    break;
                                case EbmlIds::Crc32:
                                case EbmlIds::Void:
                                    break;
                                default:
                                    addNotification(NotificationType::Warning, "\"CuePoint\"-element contains unknown element \"" + cuePointElement->idToString() + "\".", context);
                                }
                            }
                            // validate existence of mandatory elements
                            if(!cueTimeFound) {
                                addNotification(NotificationType::Warning, "\"CuePoint\"-element does not contain mandatory element \"CueTime\".", context);
                            }
                            if(!cueTrackPositionsFound) {
                                addNotification(NotificationType::Warning, "\"CuePoint\"-element does not contain mandatory element \"CueClusterPosition\".", context);
                            }
                            break;
                        default:
                            ;
                        }
                    }
                    break;
                case MatroskaIds::Cluster:
                    // parse childs of "Cluster"-element
                    for(EbmlElement *clusterElementChild = segmentChildElement->firstChild(); clusterElementChild; clusterElementChild = clusterElementChild->nextSibling()) {
                        clusterElementChild->parse();
                        switch(clusterElementChild->id()) {
                        case EbmlIds::Void:
                        case EbmlIds::Crc32:
                            break;
                        case MatroskaIds::Position:
                            // validate position
                            if((pos = clusterElementChild->readUInteger()) > 0 && (segmentChildElement->startOffset() - segmentElement->dataOffset() + currentOffset) != pos) {
                                addNotification(NotificationType::Critical, "\"Position\"-element at " + numberToString(clusterElementChild->startOffset()) + " points to " + numberToString(pos) + " which is not the offset of the containing \"Cluster\"-element.", context);
                            }
                            break;
                        case MatroskaIds::PrevSize:
                            // validate prev size
                            if(clusterElementChild->readUInteger() != prevClusterSize) {
                                addNotification(NotificationType::Critical, "\"PrevSize\"-element at " + numberToString(clusterElementChild->startOffset()) + " has invalid value.", context);
                            }
                            break;
                        default:
                            ;
                        }
                    }
                    prevClusterSize = segmentChildElement->totalSize();
                    break;
                default:
                    ;
                }
            }
            currentOffset += segmentElement->totalSize();
        }
    }
    // add a warning when no index could be found
    if(!cuesElementsFound) {
        addNotification(NotificationType::Warning, "No \"Cues\"-elements (index) found.", context);
    }
}

/*!
 * \brief Returns an indication whether \a offset equals the start offset of \a element.
 */
bool sameOffset(uint64 offset, const EbmlElement *element) {
    return element->startOffset() == offset;
}

/*!
 * \brief Returns whether none of the specified \a elements have the specified \a offset.
 * \remarks This method is used when gathering elements to avoid adding the same element twice.
 */
inline bool excludesOffset(const vector<EbmlElement *> &elements, uint64 offset)
{
    return find_if(elements.cbegin(), elements.cend(), std::bind(sameOffset, offset, _1)) == elements.cend();
}

MatroskaChapter *MatroskaContainer::chapter(std::size_t index)
{
    for(const auto &entry : m_editionEntries) {
        const auto &chapters = entry->chapters();
        if(index < chapters.size()) {
            return chapters[index].get();
        } else {
            index -= chapters.size();
        }
    }
    return nullptr;
}

size_t MatroskaContainer::chapterCount() const
{
    size_t count = 0;
    for(const auto &entry : m_editionEntries) {
        count += entry->chapters().size();
    }
    return count;
}

MatroskaAttachment *MatroskaContainer::createAttachment()
{
    // generate unique ID
    srand(time(nullptr));
    byte tries = 0;
    uint64 attachmentId;
generateRandomId:
    attachmentId = rand();
    if(tries < 0xFF) {
        for(const auto &attachment : m_attachments) {
            if(attachmentId == attachment->id()) {
                ++tries;
                goto generateRandomId;
            }
        }
    }
    // create new attachment, set ID
    m_attachments.emplace_back(make_unique<MatroskaAttachment>());
    auto &attachment = m_attachments.back();
    attachment->setId(attachmentId);
    return attachment.get();
}

ElementPosition MatroskaContainer::determineTagPosition() const
{
    return ElementPosition::Keep; // TODO
}

void MatroskaContainer::internalParseHeader()
{
    static const string context("parsing header of Matroska container");
    // reset old results
    m_firstElement = make_unique<EbmlElement>(*this, startOffset());
    m_additionalElements.clear();
    m_tracksElements.clear();
    m_segmentInfoElements.clear();
    m_tagsElements.clear();
    m_seekInfos.clear();
    m_segmentCount = 0;
    uint64 currentOffset = 0;
    vector<MatroskaSeekInfo>::size_type seekInfosIndex = 0;
    // loop through all top level elements
    for(EbmlElement *topLevelElement = m_firstElement.get(); topLevelElement; topLevelElement = topLevelElement->nextSibling()) {
        try {
            topLevelElement->parse();
            switch(topLevelElement->id()) {
            case EbmlIds::Header:
                for(EbmlElement *subElement = topLevelElement->firstChild(); subElement; subElement = subElement->nextSibling()) {
                    try {
                        subElement->parse();
                        switch(subElement->id()) {
                        case EbmlIds::Version:
                            m_version = subElement->readUInteger();
                            break;
                        case EbmlIds::ReadVersion:
                            m_readVersion = subElement->readUInteger();
                            break;
                        case EbmlIds::DocType:
                            m_doctype = subElement->readString();
                            break;
                        case EbmlIds::DocTypeVersion:
                            m_doctypeVersion = subElement->readUInteger();
                            break;
                        case EbmlIds::DocTypeReadVersion:
                            m_doctypeReadVersion = subElement->readUInteger();
                            break;
                        case EbmlIds::MaxIdLength:
                            m_maxIdLength = subElement->readUInteger();
                            if(m_maxIdLength > EbmlElement::maximumIdLengthSupported()) {
                                addNotification(NotificationType::Critical, "Maximum EBML element ID length greather then "
                                                + numberToString<uint32>(EbmlElement::maximumIdLengthSupported())
                                                + " bytes is not supported.", context);
                                throw InvalidDataException();
                            }
                            break;
                        case EbmlIds::MaxSizeLength:
                            m_maxSizeLength = subElement->readUInteger();
                            if(m_maxSizeLength > EbmlElement::maximumSizeLengthSupported()) {
                                addNotification(NotificationType::Critical, "Maximum EBML element size length greather then "
                                                + numberToString<uint32>(EbmlElement::maximumSizeLengthSupported())
                                                + " bytes is not supported.", context);
                                throw InvalidDataException();
                            }
                            break;
                        }
                        addNotifications(*subElement);
                    } catch(const Failure &) {
                        addNotifications(*subElement);
                        addNotification(NotificationType::Critical, "Unable to parse all childs of EBML header.", context);
                        break;
                    }
                }
                break;
            case MatroskaIds::Segment:
                ++m_segmentCount;
                for(EbmlElement *subElement = topLevelElement->firstChild(); subElement; subElement = subElement->nextSibling()) {
                    try {
                        subElement->parse();
                        switch(subElement->id()) {
                        case MatroskaIds::SeekHead:
                            m_seekInfos.emplace_back(make_unique<MatroskaSeekInfo>());
                            m_seekInfos.back()->parse(subElement);
                            addNotifications(*m_seekInfos.back());
                            break;
                        case MatroskaIds::Tracks:
                            if(excludesOffset(m_tracksElements, subElement->startOffset())) {
                                m_tracksElements.push_back(subElement);
                            }
                            break;
                        case MatroskaIds::SegmentInfo:
                            if(excludesOffset(m_segmentInfoElements, subElement->startOffset())) {
                                m_segmentInfoElements.push_back(subElement);
                            }
                            break;
                        case MatroskaIds::Tags:
                            if(excludesOffset(m_tagsElements, subElement->startOffset())) {
                                m_tagsElements.push_back(subElement);
                            }
                            break;
                        case MatroskaIds::Chapters:
                            if(excludesOffset(m_chaptersElements, subElement->startOffset())) {
                                m_chaptersElements.push_back(subElement);
                            }
                            break;
                        case MatroskaIds::Attachments:
                            if(excludesOffset(m_attachmentsElements, subElement->startOffset())) {
                                m_attachmentsElements.push_back(subElement);
                            }
                            break;
                        case MatroskaIds::Cluster:
                            // cluster reached
                            // stop here if all relevant information has been gathered
                            for(auto i = m_seekInfos.cbegin() + seekInfosIndex, end = m_seekInfos.cend(); i != end; ++i, ++seekInfosIndex) {
                                for(const auto &infoPair : (*i)->info()) {
                                    uint64 offset = currentOffset + topLevelElement->dataOffset() + infoPair.second;
                                    if(offset >= fileInfo().size()) {
                                        addNotification(NotificationType::Critical, "Offset (" + numberToString(offset) + ") denoted by \"SeekHead\" element is invalid.", context);
                                    } else {
                                        auto element = make_unique<EbmlElement>(*this, offset);
                                        try {
                                            element->parse();
                                            if(element->id() != infoPair.first) {
                                                addNotification(NotificationType::Critical, "ID of element " + element->idToString() + " at " + numberToString(offset) + " does not match the ID denoted in the \"SeekHead\" element (0x" + numberToString(infoPair.first, 16) + ").", context);
                                            }
                                            switch(element->id()) {
                                            case MatroskaIds::SegmentInfo:
                                                if(excludesOffset(m_segmentInfoElements, offset)) {
                                                    m_additionalElements.emplace_back(move(element));
                                                    m_segmentInfoElements.emplace_back(m_additionalElements.back().get());
                                                }
                                                break;
                                            case MatroskaIds::Tracks:
                                                if(excludesOffset(m_tracksElements, offset)) {
                                                    m_additionalElements.emplace_back(move(element));
                                                    m_tracksElements.emplace_back(m_additionalElements.back().get());
                                                }
                                                break;
                                            case MatroskaIds::Tags:
                                                if(excludesOffset(m_tagsElements, offset)) {
                                                    m_additionalElements.emplace_back(move(element));
                                                    m_tagsElements.emplace_back(m_additionalElements.back().get());
                                                }
                                                break;
                                            case MatroskaIds::Chapters:
                                                if(excludesOffset(m_chaptersElements, offset)) {
                                                    m_additionalElements.emplace_back(move(element));
                                                    m_chaptersElements.emplace_back(m_additionalElements.back().get());
                                                }
                                                break;
                                            case MatroskaIds::Attachments:
                                                if(excludesOffset(m_attachmentsElements, offset)) {
                                                    m_additionalElements.emplace_back(move(element));
                                                    m_attachmentsElements.emplace_back(m_additionalElements.back().get());
                                                }
                                                break;
                                            default:
                                                ;
                                            }
                                        } catch(const Failure &) {
                                            addNotification(NotificationType::Critical, "Can not parse element at " + numberToString(offset) + " (denoted using \"SeekHead\" element).", context);
                                        }
                                    }
                                }
                            }
                            // not checking if m_tagsElements is empty avoids long parsing times when loading big files
                            // but also has the disadvantage that the parser relies on the presence of a SeekHead element
                            // (which is not mandatory) to detect tags at the end of the segment
                            if(((!m_tracksElements.empty() && !m_tagsElements.empty()) || fileInfo().size() > m_maxFullParseSize) && !m_segmentInfoElements.empty()) {
                                goto finish;
                            }
                            break;
                        }
                        addNotifications(*subElement);
                    } catch(const Failure &) {
                        addNotifications(*subElement);
                        addNotification(NotificationType::Critical, "Unable to parse all childs of \"Segment\"-element.", context);
                        break;
                    }
                }
                currentOffset += topLevelElement->totalSize();
                break;
            default:
                ;
            }
            addNotifications(*topLevelElement);
        } catch(const Failure &) {
            addNotifications(*topLevelElement);
            addNotification(NotificationType::Critical, "Unable to parse top-level element at " + numberToString(topLevelElement->startOffset()) + ".", context);
            break;
        }
    }
    // finally parse the "Info"-element and fetch "EditionEntry"-elements
finish:
    try {
        parseSegmentInfo();
    } catch(const Failure &) {
        addNotification(NotificationType::Critical, "Unable to parse EBML (segment) \"Info\"-element.", context);
    }
}

/*!
 * \brief Parses the (segment) "Info"-element.
 *
 * This private method is called when parsing the header.
 *
 * \throws Throws std::ios_base::failure when an IO error occurs.
 * \throws Throws Media::Failure or a derived exception when a parsing
 *         error occurs.
 */
void MatroskaContainer::parseSegmentInfo()
{
    if(m_segmentInfoElements.empty()) {
        throw NoDataFoundException();
    }
    m_duration = TimeSpan();
    for(EbmlElement *element : m_segmentInfoElements) {
        element->parse();
        EbmlElement *subElement = element->firstChild();
        float64 rawDuration = 0.0;
        uint64 timeScale = 0;
        bool hasTitle = false;
        while(subElement) {
            subElement->parse();
            switch(subElement->id()) {
            case MatroskaIds::Title:
                m_titles.emplace_back(subElement->readString());
                hasTitle = true;
                break;
            case MatroskaIds::Duration:
                rawDuration = subElement->readFloat();
                break;
            case MatroskaIds::TimeCodeScale:
                timeScale = subElement->readUInteger();
                break;
            }
            subElement = subElement->nextSibling();
        }
        if(!hasTitle) {
            // add empty string as title for segment if no
            // "Title"-element has been specified
            m_titles.emplace_back();
        }
        if(rawDuration > 0.0 && timeScale > 0) {
            m_duration += TimeSpan::fromSeconds(rawDuration * timeScale / 1000000000);
        }
    }
}

void MatroskaContainer::internalParseTags()
{
    static const string context("parsing tags of Matroska container");
    for(EbmlElement *element : m_tagsElements) {
        try {
            element->parse();
            for(EbmlElement *subElement = element->firstChild(); subElement; subElement = subElement->nextSibling()) {
                subElement->parse();
                switch(subElement->id()) {
                case MatroskaIds::Tag:
                    m_tags.emplace_back(make_unique<MatroskaTag>());
                    try {
                    m_tags.back()->parse(*subElement);
                } catch(NoDataFoundException &) {
                        m_tags.pop_back();
                    } catch(const Failure &) {
                        addNotification(NotificationType::Critical, "Unable to parse tag " + ConversionUtilities::numberToString(m_tags.size()) + ".", context);
                    }
                    break;
                case EbmlIds::Crc32:
                case EbmlIds::Void:
                    break;
                default:
                    addNotification(NotificationType::Warning, "\"Tags\"-element contains unknown child. It will be ignored.", context);
                }
            }
        } catch(const Failure &) {
            addNotification(NotificationType::Critical, "Element structure seems to be invalid.", context);
            throw;
        }
    }
}

void MatroskaContainer::internalParseTracks()
{
    invalidateStatus();
    static const string context("parsing tracks of Matroska container");
    for(EbmlElement *element : m_tracksElements) {
        try {
            element->parse();
            for(EbmlElement *subElement = element->firstChild(); subElement; subElement = subElement->nextSibling()) {
                subElement->parse();
                switch(subElement->id()) {
                case MatroskaIds::TrackEntry:
                    m_tracks.emplace_back(make_unique<MatroskaTrack>(*subElement));
                    try {
                        m_tracks.back()->parseHeader();
                    } catch(const NoDataFoundException &) {
                        m_tracks.pop_back();
                    } catch(const Failure &) {
                        addNotification(NotificationType::Critical, "Unable to parse track " + ConversionUtilities::numberToString(m_tracks.size()) + ".", context);
                    }
                    break;
                case EbmlIds::Crc32:
                case EbmlIds::Void:
                    break;
                default:
                    addNotification(NotificationType::Warning, "\"Tracks\"-element contains unknown child element \"" + subElement->idToString() + "\". It will be ignored.", context);
                }
            }
        } catch(const Failure &) {
            addNotification(NotificationType::Critical, "Element structure seems to be invalid.", context);
            throw;
        }
    }
}

void MatroskaContainer::internalParseChapters()
{
    invalidateStatus();
    static const string context("parsing editions/chapters of Matroska container");
    for(EbmlElement *element : m_chaptersElements) {
        try {
            element->parse();
            for(EbmlElement *subElement = element->firstChild(); subElement; subElement = subElement->nextSibling()) {
                subElement->parse();
                switch(subElement->id()) {
                case MatroskaIds::EditionEntry:
                    m_editionEntries.emplace_back(make_unique<MatroskaEditionEntry>(subElement));
                    try {
                        m_editionEntries.back()->parseNested();
                    } catch(const NoDataFoundException &) {
                        m_editionEntries.pop_back();
                    } catch(const Failure &) {
                        addNotification(NotificationType::Critical, "Unable to parse edition entry " + ConversionUtilities::numberToString(m_editionEntries.size()) + ".", context);
                    }
                    break;
                case EbmlIds::Crc32:
                case EbmlIds::Void:
                    break;
                default:
                    addNotification(NotificationType::Warning, "\"Chapters\"-element contains unknown child element \"" + subElement->idToString() + "\". It will be ignored.", context);
                }
            }
        } catch(const Failure &) {
            addNotification(NotificationType::Critical, "Element structure seems to be invalid.", context);
            throw;
        }
    }
}

void MatroskaContainer::internalParseAttachments()
{
    invalidateStatus();
    static const string context("parsing attachments of Matroska container");
    for(EbmlElement *element : m_attachmentsElements) {
        try {
            element->parse();
            for(EbmlElement *subElement = element->firstChild(); subElement; subElement = subElement->nextSibling()) {
                subElement->parse();
                switch(subElement->id()) {
                case MatroskaIds::AttachedFile:
                    m_attachments.emplace_back(make_unique<MatroskaAttachment>());
                    try {
                        m_attachments.back()->parse(subElement);
                    } catch(const NoDataFoundException &) {
                        m_attachments.pop_back();
                    } catch(const Failure &) {
                        addNotification(NotificationType::Critical, "Unable to parse attached file " + ConversionUtilities::numberToString(m_attachments.size()) + ".", context);
                    }
                    break;
                case EbmlIds::Crc32:
                case EbmlIds::Void:
                    break;
                default:
                    addNotification(NotificationType::Warning, "\"Attachments\"-element contains unknown child element \"" + subElement->idToString() + "\". It will be ignored.", context);
                }
            }
        } catch(const Failure &) {
            addNotification(NotificationType::Critical, "Element structure seems to be invalid.", context);
            throw;
        }
    }
}

/// \brief The private SegmentData struct is used in MatroskaContainer::internalMakeFile() to store segment specific data.
struct SegmentData
{
    /// \brief Constructs a new segment data object.
    SegmentData() :
        hasCrc32(false),
        cuesElement(nullptr),
        infoDataSize(0),
        firstClusterElement(nullptr),
        clusterEndOffset(0),
        startOffset(0),
        newPadding(0),
        sizeDenotationLength(0),
        totalDataSize(0),
        totalSize(0),
        newDataOffset(0)
    {}

    /// \brief whether CRC-32 checksum is present
    bool hasCrc32;
    /// \brief used to make "SeekHead"-element
    MatroskaSeekInfo seekInfo;
    /// \brief "Cues"-element (original file)
    EbmlElement *cuesElement;
    /// \brief used to make "Cues"-element
    MatroskaCuePositionUpdater cuesUpdater;
    /// \brief size of the "SegmentInfo"-element
    uint64 infoDataSize;
    /// \brief cluster sizes
    vector<uint64> clusterSizes;
    /// \brief first "Cluster"-element (original file)
    EbmlElement *firstClusterElement;
    /// \brief end offset of last "Cluster"-element (original file)
    uint64 clusterEndOffset;
    /// \brief start offset (in the new file)
    uint64 startOffset;
    /// \brief padding (in the new file)
    uint64 newPadding;
    /// \brief header size (in the new file)
    byte sizeDenotationLength;
    /// \brief total size of the segment data (in the new file, excluding header)
    uint64 totalDataSize;
    /// \brief total size of the segment data (in the new file, including header)
    uint64 totalSize;
    /// \brief data offset of the segment in the new file
    uint64 newDataOffset;
};

void MatroskaContainer::internalMakeFile()
{
    // set initial status
    invalidateStatus();
    static const string context("making Matroska container");
    updateStatus("Calculating element sizes ...");

    // basic validation of original file
    if(!isHeaderParsed()) {
        addNotification(NotificationType::Critical, "The header has not been parsed yet.", context);
        throw InvalidDataException();
    }

    // define variables for parsing the elements of the original file
    EbmlElement *level0Element = firstElement();
    if(!level0Element) {
        addNotification(NotificationType::Critical, "No EBML elements could be found.", context);
        throw InvalidDataException();
    }
    EbmlElement *level1Element, *level2Element;

    // define variables needed for precalculation of "Tags"- and "Attachments"-element
    vector<MatroskaTagMaker> tagMaker;
    uint64 tagElementsSize = 0;
    uint64 tagsSize;
    vector<MatroskaAttachmentMaker> attachmentMaker;
    uint64 attachedFileElementsSize = 0;
    uint64 attachmentsSize;

    // define variables to store sizes, offsets and other information required to make a header and "Segment"-elements
    // current segment index
    unsigned int segmentIndex = 0;
    // segment specific data
    vector<SegmentData> segmentData;
    // offset of the segment which is currently written / offset of "Cues"-element in segment
    uint64 offset;
    // current total offset (including EBML header)
    uint64 totalOffset;
    // current write offset (used to calculate positions)
    uint64 currentPosition = 0;
    // holds the offsets of all CRC-32 elements and the length of the enclosing block
    vector<tuple<uint64, uint64> > crc32Offsets;
    // size length used to make size denotations
    byte sizeLength;
    // sizes and offsets for cluster calculation
    uint64 clusterSize, clusterReadSize, clusterReadOffset;

    // define variables needed to manage file layout
    // -> use the preferred tag position by default (might be changed later if not forced)
    ElementPosition newTagPos = fileInfo().tagPosition();
    // -> current tag position (determined later)
    ElementPosition currentTagPos = ElementPosition::Keep;
    // -> use the preferred cue position by default (might be changed later if not forced)
    ElementPosition newCuesPos = fileInfo().indexPosition();
    // --> current cue position (determined later)
    ElementPosition currentCuesPos = ElementPosition::Keep;
    // -> index of the last segment
    unsigned int lastSegmentIndex = static_cast<unsigned int>(-1);
    // -> holds new padding
    uint64 newPadding;
    // -> whether rewrite is required (always required when forced to rewrite)
    bool rewriteRequired = fileInfo().isForcingRewrite() || !fileInfo().saveFilePath().empty();

    // calculate EBML header size
    // -> sub element ID sizes
    uint64 ebmlHeaderDataSize = 2 * 7;
    // -> content and size denotation length of numeric sub elements
    for(auto headerValue : initializer_list<uint64>{m_version, m_readVersion, m_maxIdLength, m_maxSizeLength, m_doctypeVersion, m_doctypeReadVersion}) {
        ebmlHeaderDataSize += sizeLength = EbmlElement::calculateUIntegerLength(headerValue);
        ebmlHeaderDataSize += EbmlElement::calculateSizeDenotationLength(sizeLength);
    }
    // -> content and size denotation length of string sub elements
    ebmlHeaderDataSize += m_doctype.size();
    ebmlHeaderDataSize += EbmlElement::calculateSizeDenotationLength(m_doctype.size());
    uint64 ebmlHeaderSize = 4 + EbmlElement::calculateSizeDenotationLength(ebmlHeaderDataSize) + ebmlHeaderDataSize;

    try {
        // calculate size of "Tags"-element
        for(auto &tag : tags()) {
            tag->invalidateNotifications();
            try {
                tagMaker.emplace_back(tag->prepareMaking());
                if(tagMaker.back().requiredSize() > 3) {
                    // a tag of 3 bytes size is empty and can be skipped
                    tagElementsSize += tagMaker.back().requiredSize();
                }
            } catch(const Failure &) {
                // nothing to do because notifications will be added anyways
            }
            addNotifications(*tag);
        }
        tagsSize = tagElementsSize ? 4 + EbmlElement::calculateSizeDenotationLength(tagElementsSize) + tagElementsSize : 0;

        // calculate size of "Attachments"-element
        for(auto &attachment : m_attachments) {
            if(!attachment->isIgnored()) {
                attachment->invalidateNotifications();
                try {
                    attachmentMaker.emplace_back(attachment->prepareMaking());
                    if(attachmentMaker.back().requiredSize() > 3) {
                        // an attachment of 3 bytes size is empty and can be skipped
                        attachedFileElementsSize += attachmentMaker.back().requiredSize();
                    }
                } catch(const Failure &) {
                    // nothing to do because notifications will be added anyways
                }
                addNotifications(*attachment);
            }
        }
        attachmentsSize = attachedFileElementsSize ? 4 + EbmlElement::calculateSizeDenotationLength(attachedFileElementsSize) + attachedFileElementsSize : 0;

        // inspect layout of original file
        //  - number of segments
        //  - position of tags relative to the media data
        try {
            for(bool firstClusterFound = false, firstTagFound = false; level0Element; level0Element = level0Element->nextSibling()) {
                level0Element->parse();
                switch(level0Element->id()) {
                case MatroskaIds::Segment:
                    ++lastSegmentIndex;
                    for(level1Element = level0Element->firstChild(); level1Element && !firstClusterFound && !firstTagFound; level1Element = level1Element->nextSibling()) {
                        level1Element->parse();
                        switch(level1Element->id()) {
                        case MatroskaIds::Tags:
                        case MatroskaIds::Attachments:
                            firstTagFound = true;
                            break;
                        case MatroskaIds::Cluster:
                            firstClusterFound = true;
                        }
                    }
                    if(firstTagFound) {
                        currentTagPos = ElementPosition::BeforeData;
                    } else if(firstClusterFound) {
                        currentTagPos = ElementPosition::AfterData;
                    }
                }
            }

            // now the number of segments is known -> allocate segment specific data
            segmentData.resize(lastSegmentIndex + 1);

            // now the current tag/cue position might be known
            if(newTagPos == ElementPosition::Keep) {
                if((newTagPos = currentTagPos) == ElementPosition::Keep) {
                    newTagPos = ElementPosition::BeforeData;
                }
            }

        } catch(const Failure &) {
            addNotification(NotificationType::Critical, "Unable to parse content in top-level element at " + numberToString(level0Element->startOffset()) + " of original file.", context);
            throw;
        }

calculateSegmentData:
        // define variables to store sizes, offsets and other information required to make a header and "Segment"-elements
        // -> current "pretent" write offset
        uint64 currentOffset = ebmlHeaderSize;
        // -> current read offset (used to calculate positions)
        uint64 readOffset = 0;
        // -> index of current element during iteration
        unsigned int index;

        // if rewriting is required always use the preferred tag/cue position
        if(rewriteRequired) {
            newTagPos = fileInfo().tagPosition();
            if(newTagPos == ElementPosition::Keep) {
                if((newTagPos = currentTagPos) == ElementPosition::Keep) {
                    newTagPos = ElementPosition::BeforeData;
                }
            }
            newCuesPos = fileInfo().indexPosition();
        }

        // calculate sizes and other information required to make segments
        updateStatus("Calculating segment data ...", 0.0);
        for(level0Element = firstElement(), currentPosition = newPadding = segmentIndex = 0; level0Element; level0Element = level0Element->nextSibling()) {
            switch(level0Element->id()) {
            case EbmlIds::Header:
                // header size has already been calculated
                break;

            case EbmlIds::Void:
            case EbmlIds::Crc32:
                // level 0 "Void"- and "Checksum"-elements are omitted
                break;

            case MatroskaIds::Segment: {
                // get reference to the current segment data instance
                SegmentData &segment = segmentData[segmentIndex];

                // parse original "Cues"-element (if present)
                if(!segment.cuesElement) {
                    if((segment.cuesElement = level0Element->childById(MatroskaIds::Cues))) {
                        try {
                            segment.cuesUpdater.parse(segment.cuesElement);
                        } catch(const Failure &) {
                            addNotifications(segment.cuesUpdater);
                            throw;
                        }
                        addNotifications(segment.cuesUpdater);
                    }
                }

                // get first "Cluster"-element
                if(!segment.firstClusterElement) {
                    segment.firstClusterElement = level0Element->childById(MatroskaIds::Cluster);
                }

                // determine current/new cue position
                if(segment.cuesElement && segment.firstClusterElement) {
                    currentCuesPos = segment.cuesElement->startOffset() < segment.firstClusterElement->startOffset() ? ElementPosition::BeforeData : ElementPosition::AfterData;
                    if(newCuesPos == ElementPosition::Keep) {
                        newCuesPos = currentCuesPos;
                    }
                } else if(newCuesPos == ElementPosition::Keep) {
                    newCuesPos = ElementPosition::BeforeData;
                }

                // set start offset of the segment in the new file
                segment.startOffset = currentOffset;

                // check whether the segment has a CRC-32 element
                segment.hasCrc32 = level0Element->firstChild() && level0Element->firstChild()->id() == EbmlIds::Crc32;

                // precalculate the size of the segment
calculateSegmentSize:

                // pretent writing "CRC-32"-element (which either present and 6 byte long or omitted)
                segment.totalDataSize = segment.hasCrc32 ? 6 : 0;

                // pretend writing "SeekHead"-element
                segment.totalDataSize += segment.seekInfo.actualSize();

                // pretend writing "SegmentInfo"-element
                for(level1Element = level0Element->childById(MatroskaIds::SegmentInfo), index = 0; level1Element; level1Element = level1Element->siblingById(MatroskaIds::SegmentInfo), ++index) {
                    // update offset in "SeekHead"-element
                    if(segment.seekInfo.push(index, MatroskaIds::SegmentInfo, currentPosition + segment.totalDataSize)) {
                        goto calculateSegmentSize;
                    } else {
                        // add size of "SegmentInfo"-element
                        // -> size of "MuxingApp"- and "WritingApp"-element
                        segment.infoDataSize = 2 * appInfoElementTotalSize;
                        // -> add size of "Title"-element
                        if(segmentIndex < m_titles.size()) {
                            const auto &title = m_titles[segmentIndex];
                            if(!title.empty()) {
                                segment.infoDataSize += 2 + EbmlElement::calculateSizeDenotationLength(title.size()) + title.size();
                            }
                        }
                        // -> add size of other childs
                        for(level2Element = level1Element->firstChild(); level2Element; level2Element = level2Element->nextSibling()) {
                            level2Element->parse();
                            switch(level2Element->id()) {
                            case EbmlIds::Void: // skipped
                            case EbmlIds::Crc32: // skipped
                            case MatroskaIds::Title: // calculated separately
                            case MatroskaIds::MuxingApp: // calculated separately
                            case MatroskaIds::WrittingApp: // calculated separately
                                break;
                            default:
                                level2Element->makeBuffer();
                                segment.infoDataSize += level2Element->totalSize();
                            }
                        }
                        // -> calculate total size
                        segment.totalDataSize += 4 + EbmlElement::calculateSizeDenotationLength(segment.infoDataSize) + segment.infoDataSize;
                    }
                }

                // pretend writing "Tracks"- and "Chapters"-element
                for(const auto id : initializer_list<EbmlElement::identifierType>{MatroskaIds::Tracks, MatroskaIds::Chapters}) {
                    for(level1Element = level0Element->childById(id), index = 0; level1Element; level1Element = level1Element->siblingById(id), ++index) {
                        // update offset in "SeekHead"-element
                        if(segment.seekInfo.push(index, id, currentPosition + segment.totalDataSize)) {
                            goto calculateSegmentSize;
                        } else {
                            // add size of element
                            level1Element->makeBuffer();
                            segment.totalDataSize += level1Element->totalSize();
                        }
                    }
                }

                // "Tags"- and "Attachments"-element are written in either the first or the last segment
                // and either before "Cues"- and "Cluster"-elements or after these elements
                // depending on the desired tag position (at the front/at the end)
                if(newTagPos == ElementPosition::BeforeData && segmentIndex == 0) {
                    // pretend writing "Tags"-element
                    if(tagsSize) {
                        // update offsets in "SeekHead"-element
                        if(segment.seekInfo.push(0, MatroskaIds::Tags, currentPosition + segment.totalDataSize)) {
                            goto calculateSegmentSize;
                        } else {
                            // add size of "Tags"-element
                            segment.totalDataSize += tagsSize;
                        }
                    }
                    // pretend writing "Attachments"-element
                    if(attachmentsSize) {
                        // update offsets in "SeekHead"-element
                        if(segment.seekInfo.push(0, MatroskaIds::Attachments, currentPosition + segment.totalDataSize)) {
                            goto calculateSegmentSize;
                        } else {
                            // add size of "Attachments"-element
                            segment.totalDataSize += attachmentsSize;
                        }
                    }
                }

                offset = segment.totalDataSize; // save current offset (offset before "Cues"-element)

                // pretend writing "Cues"-element
                if(newCuesPos == ElementPosition::BeforeData && segment.cuesElement) {
                    // update offset of "Cues"-element in "SeekHead"-element
                    if(segment.seekInfo.push(0, MatroskaIds::Cues, currentPosition + segment.totalDataSize)) {
                        goto calculateSegmentSize;
                    } else {
                        // add size of "Cues"-element
addCuesElementSize:
                        segment.totalDataSize += segment.cuesUpdater.totalSize();
                    }
                }

                // decided whether it is necessary to rewrite the entire file (if not already rewriting)
                if(!rewriteRequired) {
                    // -> find first "Cluster"-element
                    if((level1Element = segment.firstClusterElement)) {
                        // there is at least one "Cluster"-element to be written
                        //if(level1Element->startOffset() == currentFirstClusterOffset) {
                            // just before the first "Cluster"-element
                            // -> calculate total offset (excluding size denotation and incomplete index)
                            totalOffset = currentOffset + 4 + segment.totalDataSize;

                            if(totalOffset <= segment.firstClusterElement->startOffset()) {
                                // the padding might be big enough, but
                                // - the segment might become bigger (subsequent tags and attachments)
                                // - the header size hasn't been taken into account yet
                                // - seek information for first cluster and subsequent tags and attachments hasn't been taken into account

                                // assume the size denotation length doesn't change -> use length from original file
                                if(level0Element->headerSize() <= 4 || level0Element->headerSize() > 12) {
                                    // validate original header size
                                    addNotification(NotificationType::Critical, "Header size of \"Segment\"-element from original file is invalid.", context);
                                    throw InvalidDataException();
                                }
                                segment.sizeDenotationLength = level0Element->headerSize() - 4;

nonRewriteCalculations:
                                // pretend writing "Cluster"-elements assuming there is no rewrite required
                                // -> update offset in "SeakHead"-element
                                if(segment.seekInfo.push(0, MatroskaIds::Cluster, level1Element->startOffset() - 4 - segment.sizeDenotationLength - ebmlHeaderSize)) {
                                    goto calculateSegmentSize;
                                }
                                // -> update offset of "Cluster"-element in "Cues"-element and get end offset of last "Cluster"-element
                                for(; level1Element; level1Element = level1Element->siblingById(MatroskaIds::Cluster)) {
                                    clusterReadOffset = level1Element->startOffset() - level0Element->dataOffset() + readOffset;
                                    segment.clusterEndOffset = level1Element->endOffset();
                                    if(segment.cuesElement && segment.cuesUpdater.updateOffsets(clusterReadOffset, level1Element->startOffset() - 4 - segment.sizeDenotationLength - ebmlHeaderSize) && newCuesPos == ElementPosition::BeforeData) {
                                        segment.totalDataSize = offset;
                                        goto addCuesElementSize;
                                    }
                                }
                                segment.totalDataSize = segment.clusterEndOffset - currentOffset - 4 - segment.sizeDenotationLength;

                                // pretend writing "Cues"-element
                                if(newCuesPos == ElementPosition::AfterData && segment.cuesElement) {
                                    // update offset of "Cues"-element in "SeekHead"-element
                                    if(segment.seekInfo.push(0, MatroskaIds::Cues, currentPosition + segment.totalDataSize)) {
                                        goto calculateSegmentSize;
                                    } else {
                                        // add size of "Cues"-element
                                        segment.totalDataSize += segment.cuesUpdater.totalSize();
                                    }
                                }

                                if(newTagPos == ElementPosition::AfterData && segmentIndex == lastSegmentIndex) {
                                    // pretend writing "Tags"-element
                                    if(tagsSize) {
                                        // update offsets in "SeekHead"-element
                                        if(segment.seekInfo.push(0, MatroskaIds::Tags, currentPosition + segment.totalDataSize)) {
                                            goto calculateSegmentSize;
                                        } else {
                                            // add size of "Tags"-element
                                            segment.totalDataSize += tagsSize;
                                        }
                                    }
                                    // pretend writing "Attachments"-element
                                    if(attachmentsSize) {
                                        // update offsets in "SeekHead"-element
                                        if(segment.seekInfo.push(0, MatroskaIds::Attachments, currentPosition + segment.totalDataSize)) {
                                            goto calculateSegmentSize;
                                        } else {
                                            // add size of "Attachments"-element
                                            segment.totalDataSize += attachmentsSize;
                                        }
                                    }
                                }

                                // calculate total offset again (taking everything into account)
                                // -> check whether assumed size denotation was correct
                                if(segment.sizeDenotationLength != (sizeLength = EbmlElement::calculateSizeDenotationLength(segment.totalDataSize))) {
                                    // assumption was wrong -> recalculate with new length
                                    segment.sizeDenotationLength = sizeLength;
                                    level1Element = segment.firstClusterElement;
                                    goto nonRewriteCalculations;
                                }

                                totalOffset = currentOffset + 4 + sizeLength + offset;
                                // offset does not include size of "Cues"-element
                                if(newCuesPos == ElementPosition::BeforeData) {
                                    totalOffset += segment.cuesUpdater.totalSize();
                                }
                                if(totalOffset <= segment.firstClusterElement->startOffset()) {
                                    // calculate new padding
                                    if(segment.newPadding != 1) {
                                        // "Void"-element is at least 2 byte long -> can't add 1 byte padding
                                        newPadding += (segment.newPadding = segment.firstClusterElement->startOffset() - totalOffset);
                                    } else {
                                        rewriteRequired = true;
                                    }
                                } else {
                                    rewriteRequired = true;
                                }
                            } else {
                                rewriteRequired = true;
                            }
                        //} else {
                            // first "Cluster"-element in the "Segment"-element but not the first "Cluster"-element in the file
                            // TODO / nothing to do?
                        //}

                    } else {
                        // there are no "Cluster"-elements in the current "Segment"-element
                        // TODO / nothing to do?
                    }

                    if(rewriteRequired) {
                        if(newTagPos != ElementPosition::AfterData && (!fileInfo().forceTagPosition() || (fileInfo().tagPosition() == ElementPosition::Keep && currentTagPos == ElementPosition::Keep))) {
                            // rewriting might be avoided by writing the tags at the end
                            newTagPos = ElementPosition::AfterData;
                            rewriteRequired = false;
                        } else if(newCuesPos != ElementPosition::AfterData && (!fileInfo().forceIndexPosition() || (fileInfo().indexPosition() == ElementPosition::Keep && currentCuesPos == ElementPosition::Keep))) {
                            // rewriting might be avoided by writing the cues at the end
                            newCuesPos = ElementPosition::AfterData;
                            rewriteRequired = false;
                        }
                        // do calculations again for rewriting / changed element order
                        goto calculateSegmentData;
                    }
                } else {
                    // if rewrite is required pretend writing the remaining elements to compute total segment size

                    // pretend writing "Void"-element (only if there is at least one "Cluster"-element in the segment)
                    if(!segmentIndex && rewriteRequired && (level1Element = level0Element->childById(MatroskaIds::Cluster))) {
                        // simply use the preferred padding
                        segment.totalDataSize += (segment.newPadding = newPadding = fileInfo().preferredPadding());
                    }

                    // pretend writing "Cluster"-element
                    segment.clusterSizes.clear();
                    for(index = 0; level1Element; level1Element = level1Element->siblingById(MatroskaIds::Cluster), ++index) {
                        // update offset of "Cluster"-element in "Cues"-element
                        clusterReadOffset = level1Element->startOffset() - level0Element->dataOffset() + readOffset;
                        if(segment.cuesElement && segment.cuesUpdater.updateOffsets(clusterReadOffset, currentPosition + segment.totalDataSize) && newCuesPos == ElementPosition::BeforeData) {
                            segment.totalDataSize = offset; // reset element size to previously saved offset of "Cues"-element
                            goto addCuesElementSize;
                        } else {
                            if(index == 0 && segment.seekInfo.push(index, MatroskaIds::Cluster, currentPosition + segment.totalDataSize)) {
                                goto calculateSegmentSize;
                            } else {
                                // add size of "Cluster"-element
                                clusterSize = clusterReadSize = 0;
                                for(level2Element = level1Element->firstChild(); level2Element; level2Element = level2Element->nextSibling()) {
                                    level2Element->parse();
                                    if(segment.cuesElement && segment.cuesUpdater.updateRelativeOffsets(clusterReadOffset, clusterReadSize, clusterSize) && newCuesPos == ElementPosition::BeforeData) {
                                        segment.totalDataSize = offset;
                                        goto addCuesElementSize;
                                    }
                                    switch(level2Element->id()) {
                                    case EbmlIds::Void:
                                    case EbmlIds::Crc32:
                                        break;
                                    case MatroskaIds::Position:
                                        clusterSize += 1 + 1 + EbmlElement::calculateUIntegerLength(currentPosition + segment.totalDataSize);
                                        break;
                                    default:
                                        clusterSize += level2Element->totalSize();
                                    }
                                    clusterReadSize += level2Element->totalSize();
                                }
                                segment.clusterSizes.push_back(clusterSize);
                                segment.totalDataSize += 4 + EbmlElement::calculateSizeDenotationLength(clusterSize) + clusterSize;
                            }
                        }
                    }

                    // pretend writing "Cues"-element
                    if(newCuesPos == ElementPosition::AfterData && segment.cuesElement) {
                        // update offset of "Cues"-element in "SeekHead"-element
                        if(segment.seekInfo.push(0, MatroskaIds::Cues, currentPosition + segment.totalDataSize)) {
                            goto calculateSegmentSize;
                        } else {
                            // add size of "Cues"-element
                            segment.totalDataSize += segment.cuesUpdater.totalSize();
                        }
                    }

                    // "Tags"- and "Attachments"-element are written in either the first or the last segment
                    // and either before "Cues"- and "Cluster"-elements or after these elements
                    // depending on the desired tag position (at the front/at the end)
                    if(newTagPos == ElementPosition::AfterData && segmentIndex == lastSegmentIndex) {
                        // pretend writing "Tags"-element
                        if(tagsSize) {
                            // update offsets in "SeekHead"-element
                            if(segment.seekInfo.push(0, MatroskaIds::Tags, currentPosition + segment.totalDataSize)) {
                                goto calculateSegmentSize;
                            } else {
                                // add size of "Tags"-element
                                segment.totalDataSize += tagsSize;
                            }
                        }
                        // pretend writing "Attachments"-element
                        if(attachmentsSize) {
                            // update offsets in "SeekHead"-element
                            if(segment.seekInfo.push(0, MatroskaIds::Attachments, currentPosition + segment.totalDataSize)) {
                                goto calculateSegmentSize;
                            } else {
                                // add size of "Attachments"-element
                                segment.totalDataSize += attachmentsSize;
                            }
                        }
                    }
                }

                // increase the current segment index
                ++segmentIndex;

                // increase write offsets by the size of the segment which size has just been computed
                segment.totalSize = 4 + EbmlElement::calculateSizeDenotationLength(segment.totalDataSize) + segment.totalDataSize;
                currentPosition += segment.totalSize;
                currentOffset += segment.totalSize;

                // increase the read offset by the size of the segment read from the orignial file
                readOffset += level0Element->totalSize();

                break;

            } default:
                // just copy any unknown top-level elements
                addNotification(NotificationType::Warning, "The top-level element \"" + level0Element->idToString() + "\" of the original file is unknown and will just be copied.", context);
                currentOffset += level0Element->totalSize();
                readOffset += level0Element->totalSize();
            }
        }

        if(!rewriteRequired) {
            // check whether the new padding is ok according to specifications
            if((rewriteRequired = (newPadding > fileInfo().maxPadding() || newPadding < fileInfo().minPadding()))) {
                // need to recalculate segment data for rewrite
                goto calculateSegmentData;
            }
        }

    } catch(const Failure &) {
        addNotification(NotificationType::Critical, "Parsing the original file failed.", context);
        throw;
    } catch(...) {
        const char *what = catchIoFailure();
        addNotification(NotificationType::Critical, "An IO error occured when parsing the original file.", context);
        throwIoFailure(what);
    }

    if(isAborted()) {
        throw OperationAbortedException();
    }

    // setup stream(s) for writing
    // -> update status
    updateStatus("Preparing streams ...");

    // -> define variables needed to handle output stream and backup stream (required when rewriting the file)
    string backupPath;
    fstream &outputStream = fileInfo().stream();
    fstream backupStream; // create a stream to open the backup/original file for the case rewriting the file is required
    BinaryWriter outputWriter(&outputStream);
    char buff[8]; // buffer used to make size denotations

    if(rewriteRequired) {
        if(fileInfo().saveFilePath().empty()) {
            // move current file to temp dir and reopen it as backupStream, recreate original file
            try {
                BackupHelper::createBackupFile(fileInfo().path(), backupPath, outputStream, backupStream);
                // recreate original file, define buffer variables
                outputStream.open(fileInfo().path(), ios_base::out | ios_base::binary | ios_base::trunc);
            } catch(...) {
                const char *what = catchIoFailure();
                addNotification(NotificationType::Critical, "Creation of temporary file (to rewrite the original file) failed.", context);
                throwIoFailure(what);
            }
        } else {
            // open the current file as backupStream and create a new outputStream at the specified "save file path"
            try {
                backupStream.exceptions(ios_base::badbit | ios_base::failbit);
                backupStream.open(fileInfo().path(), ios_base::in | ios_base::binary);
                fileInfo().close();
                outputStream.open(fileInfo().saveFilePath(), ios_base::out | ios_base::binary | ios_base::trunc);
            } catch(...) {
                const char *what = catchIoFailure();
                addNotification(NotificationType::Critical, "Opening streams to write output file failed.", context);
                throwIoFailure(what);
            }
        }

        // set backup stream as associated input stream since we need the original elements to write the new file
        setStream(backupStream);

        // TODO: reduce code duplication

    } else { // !rewriteRequired
        // buffer currently assigned attachments
        for(auto &maker : attachmentMaker) {
            maker.bufferCurrentAttachments();
        }

        // reopen original file to ensure it is opened for writing
        try {
            fileInfo().close();
            outputStream.open(fileInfo().path(), ios_base::in | ios_base::out | ios_base::binary);
        } catch(...) {
            const char *what = catchIoFailure();
            addNotification(NotificationType::Critical, "Opening the file with write permissions failed.", context);
            throwIoFailure(what);
        }
    }

    // start actual writing
    try {
        // write EBML header
        updateStatus("Writing EBML header ...");
        outputWriter.writeUInt32BE(EbmlIds::Header);
        sizeLength = EbmlElement::makeSizeDenotation(ebmlHeaderDataSize, buff);
        outputStream.write(buff, sizeLength);
        EbmlElement::makeSimpleElement(outputStream, EbmlIds::Version, m_version);
        EbmlElement::makeSimpleElement(outputStream, EbmlIds::ReadVersion, m_readVersion);
        EbmlElement::makeSimpleElement(outputStream, EbmlIds::MaxIdLength, m_maxIdLength);
        EbmlElement::makeSimpleElement(outputStream, EbmlIds::MaxSizeLength, m_maxSizeLength);
        EbmlElement::makeSimpleElement(outputStream, EbmlIds::DocType, m_doctype);
        EbmlElement::makeSimpleElement(outputStream, EbmlIds::DocTypeVersion, m_doctypeVersion);
        EbmlElement::makeSimpleElement(outputStream, EbmlIds::DocTypeReadVersion, m_doctypeReadVersion);

        // iterates through all level 0 elements of the original file
        for(level0Element = firstElement(), segmentIndex = 0, currentPosition = 0; level0Element; level0Element = level0Element->nextSibling()) {

            // write all level 0 elements of the original file
            switch(level0Element->id()) {
            case EbmlIds::Header:
                // header has already been written -> skip it here
                break;

            case EbmlIds::Void:
            case EbmlIds::Crc32:
                // level 0 "Void"- and "Checksum"-elements are omitted
                break;

            case MatroskaIds::Segment: {
                // get reference to the current segment data instance
                SegmentData &segment = segmentData[segmentIndex];

                // write "Segment"-element actually
                updateStatus("Writing segment header ...");
                outputWriter.writeUInt32BE(MatroskaIds::Segment);
                sizeLength = EbmlElement::makeSizeDenotation(segment.totalDataSize, buff);
                outputStream.write(buff, sizeLength);
                segment.newDataOffset = offset = outputStream.tellp(); // store segment data offset here

                // write CRC-32 element ...
                if(segment.hasCrc32) {
                    // ... if the original element had a CRC-32 element
                    *buff = EbmlIds::Crc32;
                    *(buff + 1) = 0x84; // length denotation: 4 byte
                    // set the value after writing the element
                    crc32Offsets.emplace_back(outputStream.tellp(), segment.totalDataSize);
                    outputStream.write(buff, 6);
                }

                // write "SeekHead"-element (except there is no seek information for the current segment)
                segment.seekInfo.invalidateNotifications();
                segment.seekInfo.make(outputStream);
                addNotifications(segment.seekInfo);

                // write "SegmentInfo"-element
                for(level1Element = level0Element->childById(MatroskaIds::SegmentInfo); level1Element; level1Element = level1Element->siblingById(MatroskaIds::SegmentInfo)) {
                    // -> write ID and size
                    outputWriter.writeUInt32BE(MatroskaIds::SegmentInfo);
                    sizeLength = EbmlElement::makeSizeDenotation(segment.infoDataSize, buff);
                    outputStream.write(buff, sizeLength);
                    // -> write childs
                    for(level2Element = level1Element->firstChild(); level2Element; level2Element = level2Element->nextSibling()) {
                        switch(level2Element->id()) {
                        case EbmlIds::Void: // skipped
                        case EbmlIds::Crc32: // skipped
                        case MatroskaIds::Title: // written separately
                        case MatroskaIds::MuxingApp: // written separately
                        case MatroskaIds::WrittingApp: // written separately
                            break;
                        default:
                            level2Element->copyBuffer(outputStream);
                            level2Element->discardBuffer();
                            //level2Element->copyEntirely(outputStream);
                        }
                    }
                    // -> write "Title"-element
                    if(segmentIndex < m_titles.size()) {
                        const auto &title = m_titles[segmentIndex];
                        if(!title.empty()) {
                            EbmlElement::makeSimpleElement(outputStream, MatroskaIds::Title, title);
                        }
                    }
                    // -> write "MuxingApp"- and "WritingApp"-element
                    EbmlElement::makeSimpleElement(outputStream, MatroskaIds::MuxingApp, appInfo, appInfoElementDataSize);
                    EbmlElement::makeSimpleElement(outputStream, MatroskaIds::WrittingApp, appInfo, appInfoElementDataSize);
                }

                // write "Tracks"- and "Chapters"-element
                for(const auto id : initializer_list<EbmlElement::identifierType>{MatroskaIds::Tracks, MatroskaIds::Chapters}) {
                    for(level1Element = level0Element->childById(id); level1Element; level1Element = level1Element->siblingById(id)) {
                        level1Element->copyBuffer(outputStream);
                        level1Element->discardBuffer();
                        //level1Element->copyEntirely(outputStream);
                    }
                }

                if(newTagPos == ElementPosition::BeforeData && segmentIndex == 0) {
                    // write "Tags"-element
                    if(tagsSize) {
                        outputWriter.writeUInt32BE(MatroskaIds::Tags);
                        sizeLength = EbmlElement::makeSizeDenotation(tagElementsSize, buff);
                        outputStream.write(buff, sizeLength);
                        for(auto &maker : tagMaker) {
                            maker.make(outputStream);
                        }
                        // no need to add notifications; this has been done when creating the make
                    }
                    // write "Attachments"-element
                    if(attachmentsSize) {
                        outputWriter.writeUInt32BE(MatroskaIds::Attachments);
                        sizeLength = EbmlElement::makeSizeDenotation(attachedFileElementsSize, buff);
                        outputStream.write(buff, sizeLength);
                        for(auto &maker : attachmentMaker) {
                            maker.make(outputStream);
                        }
                        // no need to add notifications; this has been done when creating the make
                    }
                }

                // write "Cues"-element
                if(newCuesPos == ElementPosition::BeforeData && segment.cuesElement) {
                    try {
                        segment.cuesUpdater.make(outputStream);
                        addNotifications(segment.cuesUpdater);
                    } catch(const Failure &) {
                        addNotifications(segment.cuesUpdater);
                        throw;
                    }
                }

                // write padding / "Void"-element
                if(segment.newPadding) {

                    // calculate length
                    uint64 voidLength;
                    if(segment.newPadding < 64) {
                        sizeLength = 1;
                        *buff = (voidLength = segment.newPadding - 2) | 0x80;
                    } else {
                        sizeLength = 8;
                        BE::getBytes(static_cast<uint64>((voidLength = segment.newPadding - 9) | 0x100000000000000), buff);
                    }
                    // write header
                    outputWriter.writeByte(EbmlIds::Void);
                    outputStream.write(buff, sizeLength);
                    // write zeroes
                    for(; voidLength; --voidLength) {
                        outputStream.put(0);
                    }
                }

                // write media data / "Cluster"-elements
                level1Element = level0Element->childById(MatroskaIds::Cluster);
                if(rewriteRequired) {

                    // update status, check whether the operation has been aborted
                    if(isAborted()) {
                        throw OperationAbortedException();
                    }
                    updateStatus("Writing clusters ...", static_cast<double>(static_cast<uint64>(outputStream.tellp()) - offset) / segment.totalDataSize);
                    // write "Cluster"-element
                    for(auto clusterSizesIterator = segment.clusterSizes.cbegin();
                        level1Element; level1Element = level1Element->siblingById(MatroskaIds::Cluster), ++clusterSizesIterator) {
                        // calculate position of cluster in segment
                        clusterSize = currentPosition + (static_cast<uint64>(outputStream.tellp()) - offset);
                        // write header; checking whether clusterSizesIterator is valid shouldn't be necessary
                        outputWriter.writeUInt32BE(MatroskaIds::Cluster);
                        sizeLength = EbmlElement::makeSizeDenotation(*clusterSizesIterator, buff);
                        outputStream.write(buff, sizeLength);
                        // write childs
                        for(level2Element = level1Element->firstChild(); level2Element; level2Element = level2Element->nextSibling()) {
                            switch(level2Element->id()) {
                            case EbmlIds::Void:
                            case EbmlIds::Crc32:
                                break;
                            case MatroskaIds::Position:
                                EbmlElement::makeSimpleElement(outputStream, MatroskaIds::Position, clusterSize);
                                break;
                            default:
                                level2Element->copyEntirely(outputStream);
                            }
                        }
                        // update percentage, check whether the operation has been aborted
                        if(isAborted()) {
                            throw OperationAbortedException();
                        } else {
                            updatePercentage(static_cast<double>(static_cast<uint64>(outputStream.tellp()) - offset) / segment.totalDataSize);
                        }
                    }
                } else {
                    // can't just skip existing "Cluster"-elements: "Position"-elements must be updated
                    for(; level1Element; level1Element = level1Element->nextSibling()) {
                        for(level2Element = level1Element->firstChild(); level2Element; level2Element = level2Element->nextSibling()) {
                            switch(level2Element->id()) {
                            case MatroskaIds::Position:
                                // calculate new position
                                sizeLength = EbmlElement::makeUInteger(level1Element->startOffset() - segmentData.front().newDataOffset, buff, level2Element->dataSize());
                                // new position can only applied if it doesn't need more bytes than the previous position
                                if(level2Element->dataSize() < sizeLength) {
                                    // can't update position -> void position elements ("Position"-elements seem a bit useless anyways)
                                    outputStream.seekp(level2Element->startOffset());
                                    outputStream.put(EbmlIds::Void);
                                } else {
                                    // update position
                                    outputStream.seekp(level2Element->dataOffset());
                                    outputStream.write(buff, sizeLength);
                                }
                                break;
                            default:
                                ;
                            }
                        }
                    }
                    // skip existing "Cluster"-elements
                    outputStream.seekp(segment.clusterEndOffset);
                }

                // write "Cues"-element
                if(newCuesPos == ElementPosition::AfterData && segment.cuesElement) {
                    try {
                        segment.cuesUpdater.make(outputStream);
                        addNotifications(segment.cuesUpdater);
                    } catch(const Failure &) {
                        addNotifications(segment.cuesUpdater);
                        throw;
                    }
                }

                if(newTagPos == ElementPosition::AfterData && segmentIndex == lastSegmentIndex) {
                    // write "Tags"-element
                    if(tagsSize) {
                        outputWriter.writeUInt32BE(MatroskaIds::Tags);
                        sizeLength = EbmlElement::makeSizeDenotation(tagElementsSize, buff);
                        outputStream.write(buff, sizeLength);
                        for(auto &maker : tagMaker) {
                            maker.make(outputStream);
                        }
                        // no need to add notifications; this has been done when creating the make
                    }
                    // write "Attachments"-element
                    if(attachmentsSize) {
                        outputWriter.writeUInt32BE(MatroskaIds::Attachments);
                        sizeLength = EbmlElement::makeSizeDenotation(attachedFileElementsSize, buff);
                        outputStream.write(buff, sizeLength);
                        for(auto &maker : attachmentMaker) {
                            maker.make(outputStream);
                        }
                        // no need to add notifications; this has been done when creating the make
                    }
                }

                // increase the current segment index
                ++segmentIndex;

                // increase write offsets by the size of the segment which has just been written
                currentPosition += segment.totalSize;

                break;

           } default:
                // just copy any unknown top-level elements
                level0Element->copyEntirely(outputStream);
                currentPosition += level0Element->totalSize();
            }
        }

        // reparse what is written so far
        updateStatus("Reparsing output file ...");
        if(rewriteRequired) {
            // report new size
            fileInfo().reportSizeChanged(outputStream.tellp());

            // "save as path" is now the regular path
            if(!fileInfo().saveFilePath().empty()) {
                fileInfo().reportPathChanged(fileInfo().saveFilePath());
                fileInfo().setSaveFilePath(string());
            }

            // the outputStream needs to be reopened to be able to read again
            outputStream.close();
            outputStream.open(fileInfo().path(), ios_base::in | ios_base::out | ios_base::binary);
            setStream(outputStream);
        } else {
            const auto newSize = static_cast<uint64>(outputStream.tellp());
            if(newSize < fileInfo().size()) {
                // file is smaller after the modification -> truncate
                // -> close stream before truncating
                outputStream.close();
                // -> truncate file
                if(truncate(fileInfo().path().c_str(), newSize) == 0) {
                    fileInfo().reportSizeChanged(newSize);
                } else {
                    addNotification(NotificationType::Critical, "Unable to truncate the file.", context);
                }
                // -> reopen the stream again
                outputStream.open(fileInfo().path(), ios_base::in | ios_base::out | ios_base::binary);
            } else {
                // file is longer after the modification -> just report new size
                fileInfo().reportSizeChanged(newSize);
            }
        }
        reset();
        try {
            parseHeader();
        } catch(const Failure &) {
            addNotification(NotificationType::Critical, "Unable to reparse the header of the new file.", context);
            throw;
        }

        // update CRC-32 checksums
        if(!crc32Offsets.empty()) {
            updateStatus("Updating CRC-32 checksums ...");
            for(const auto &crc32Offset : crc32Offsets) {
                outputStream.seekg(get<0>(crc32Offset) + 6);
                outputStream.seekp(get<0>(crc32Offset) + 2);
                writer().writeUInt32LE(reader().readCrc32(get<1>(crc32Offset) - 6));
            }
        }

        updatePercentage(100.0);

        // flush output stream
        outputStream.flush();

        // handle errors (which might have been occured after renaming/creating backup file)
    } catch(...) {
        BackupHelper::handleFailureAfterFileModified(fileInfo(), backupPath, outputStream, backupStream, context);
    }
}

}
