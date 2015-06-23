#ifndef MEDIA_TAGTARGET_H
#define MEDIA_TAGTARGET_H

#include <c++utilities/application/global.h>
#include <c++utilities/conversion/types.h>

#include <string>
#include <vector>

namespace Media {

/*!
 * \brief The TagTarget class stores target information.
 */
class LIB_EXPORT TagTarget
{
public:
    typedef uint64 IdType;
    typedef std::vector<IdType> IdContainerType;

    TagTarget(uint64 level = 0, IdContainerType tracks = IdContainerType(), IdContainerType chapters = IdContainerType(), IdContainerType editions = IdContainerType(), IdContainerType attachments = IdContainerType());

    uint64 level() const;
    void setLevel(uint64 level);
    const std::string &levelName() const;
    void setLevelName(const std::string &levelName);
    const IdContainerType &tracks() const;
    IdContainerType &tracks();
    const IdContainerType &chapters() const;
    IdContainerType &chapters();
    const IdContainerType &editions() const;
    IdContainerType &editions();
    const IdContainerType &attachments() const;
    IdContainerType &attachments();
    bool isEmpty() const;
    void clear();
    std::string toString() const;
    bool operator ==(const TagTarget &other) const;

private:
    uint64 m_level;
    std::string m_levelName;
    IdContainerType m_tracks;
    IdContainerType m_chapters;
    IdContainerType m_editions;
    IdContainerType m_attachments;
};

/*!
 * \brief Constructs a new TagTarget with the specified \a level, \a track, \a chapter,
 *        \a edition and \a attachment.
 */
inline TagTarget::TagTarget(uint64 level, IdContainerType tracks, IdContainerType chapters, IdContainerType editions, IdContainerType attachments) :
    m_level(level),
    m_tracks(tracks),
    m_chapters(chapters),
    m_editions(editions),
    m_attachments(attachments)
{}

/*!
 * \brief Returns the level.
 */
inline uint64 TagTarget::level() const
{
    return m_level;
}

/*!
 * \brief Sets the level.
 */
inline void TagTarget::setLevel(uint64 level)
{
    m_level = level;
}

/*!
 * \brief Returns the level name.
 */
inline const std::string &TagTarget::levelName() const
{
    return m_levelName;
}

/*!
 * \brief Sets the level name.
 */
inline void TagTarget::setLevelName(const std::string &levelName)
{
    m_levelName = levelName;
}

/*!
 * \brief Returns the tracks.
 */
inline const TagTarget::IdContainerType &TagTarget::tracks() const
{
    return m_tracks;
}

/*!
 * \brief Returns the tracks.
 */
inline TagTarget::IdContainerType &TagTarget::tracks()
{
    return m_tracks;
}

/*!
 * \brief Returns the chapters.
 */
inline const TagTarget::IdContainerType &TagTarget::chapters() const
{
    return m_chapters;
}

/*!
 * \brief Returns the chapters.
 */
inline TagTarget::IdContainerType &TagTarget::chapters()
{
    return m_chapters;
}

/*!
 * \brief Returns the editions.
 */
inline const TagTarget::IdContainerType &TagTarget::editions() const
{
    return m_editions;
}

/*!
 * \brief Returns the editions.
 */
inline TagTarget::IdContainerType &TagTarget::editions()
{
    return m_editions;
}

/*!
 * \brief Returns the attachments.
 */
inline const TagTarget::IdContainerType &TagTarget::attachments() const
{
    return m_attachments;
}

/*!
 * \brief Returns the attachments.
 */
inline TagTarget::IdContainerType &TagTarget::attachments()
{
    return m_attachments;
}

/*!
 * \brief Returns an indication whether the target is empty.
 */
inline bool TagTarget::isEmpty() const
{
    return m_level == 0
            && m_levelName.empty()
            && m_tracks.empty()
            && m_chapters.empty()
            && m_editions.empty()
            && m_attachments.empty();
}

/*!
 * \brief Clears the TagTarget.
 */
inline void TagTarget::clear()
{
    m_level = 0;
    m_levelName.clear();
    m_tracks.clear();
    m_chapters.clear();
    m_editions.clear();
    m_attachments.clear();
}

/*!
 * \brief Returns whether the tag targets are equal.
 */
inline bool TagTarget::operator ==(const TagTarget &other) const
{
    return m_level == other.m_level
            && m_levelName == other.m_levelName
            && m_tracks == other.m_tracks
            && m_chapters == other.m_chapters
            && m_editions == other.m_editions
            && m_attachments == other.m_attachments;
}

}

#endif // MEDIA_TAGTARGET_H