#ifndef MEDIA_OGGITERATOR_H
#define MEDIA_OGGITERATOR_H

#include "oggpage.h"

#include <istream>
#include <vector>

namespace Media {

class LIB_EXPORT OggIterator
{
public:
    OggIterator(std::istream &stream, uint64 startOffset, uint64 streamSize);

    std::istream &stream();
    void setStream(std::istream &stream);
    void reset();
    void nextPage();
    void nextSegment();
    void previousPage();
    void previousSegment();
    const std::vector<OggPage> &pages() const;
    const OggPage &currentPage() const;
    std::vector<OggPage>::size_type currentPageIndex() const;
    void setPageIndex(std::vector<OggPage>::size_type index);
    void setSegmentIndex(std::vector<uint32>::size_type index);
    std::vector<uint32>::size_type currentSegmentIndex() const;
    uint64 currentSegmentOffset() const;
    uint64 currentCharacterOffset() const;
    uint32 currentSegmentSize() const;
    void setFilter(uint32 streamSerialId);
    void removeFilter();
    bool areAllPagesFetched() const;
    void read(char *buffer, size_t count);
    void seekForward(size_t count);

    operator bool() const;
    OggIterator &operator ++();
    OggIterator operator ++(int);
    OggIterator &operator --();
    OggIterator operator --(int);

private:
    bool fetchNextPage();
    bool matchesFilter(const OggPage &page);

    std::istream *m_stream;
    uint64 m_startOffset;
    uint64 m_streamSize;
    std::vector<OggPage> m_pages;
    std::vector<OggPage>::size_type m_page;
    std::vector<uint32>::size_type m_segment;
    uint64 m_offset;
    uint32 m_bytesRead;
    bool m_hasIdFilter;
    uint32 m_idFilter;
};

/*!
 * \brief Constructs a new iterator for the specified \a stream of \a streamSize bytes at the specified \a startOffset.
 */
inline OggIterator::OggIterator(std::istream &stream, uint64 startOffset, uint64 streamSize) :
    m_stream(&stream),
    m_startOffset(startOffset),
    m_streamSize(streamSize),
    m_page(0),
    m_segment(0),
    m_offset(0),
    m_bytesRead(0),
    m_hasIdFilter(false),
    m_idFilter(0)
{}

/*!
 * \brief Returns the stream.
 *
 * The stream has been specified when constructing the iterator and might be changed using the setStream() methods.
 */
inline std::istream &OggIterator::stream()
{
    return *m_stream;
}

/*!
 * \brief Sets the stream.
 * \remarks The new stream must have the same data as the old stream to keep the iterator in a sane state.
 * \sa stream()
 */
inline void OggIterator::setStream(std::istream &stream)
{
    m_stream = &stream;
}

/*!
 * \brief Returns a vector of containing the OGG pages that have been fetched yet.
 */
inline const std::vector<OggPage> &OggIterator::pages() const
{
    return m_pages;
}

/*!
 * \brief Returns the current OGG page.
 * \remarks Calling this method when the iterator is invalid causes undefined behaviour.
 */
inline const OggPage &OggIterator::currentPage() const
{
    return m_pages[m_page];
}

/*!
 * \brief Returns an indication whether the iterator is valid.
 *
 * The iterator is invalid when it has just been constructed. Incrementing and decrementing
 * might cause invalidation.
 *
 * If the iterator is invalid, it can be reseted using the reset() method.
 *
 * Some methods might cause undefined behaviour if called on an invalid iterator.
 */
inline OggIterator::operator bool() const
{
    return m_page < m_pages.size() && m_segment < m_pages[m_page].segmentSizes().size();
}

/*!
 * \brief Returns the index of the current page if the iterator is valid; otherwise an undefined index is returned.
 */
inline std::vector<OggPage>::size_type OggIterator::currentPageIndex() const
{
    return m_page;
}

/*!
 * \brief Sets the current page index.
 *
 * This method should never be called with an \a index out of range (which is the defined by the number of fetched pages), since this causes undefined behaviour.
 */
inline void OggIterator::setPageIndex(std::vector<OggPage>::size_type index)
{
    const OggPage &page = m_pages[m_page = index];
    m_segment = 0;
    m_offset = page.startOffset() + page.headerSize();
}

/*!
 * \brief Sets the current segment index.
 *
 * This method should never be called with an \a index out of range (which is defined by the number of segments in the current page), since this causes undefined behaviour.
 */
inline void OggIterator::setSegmentIndex(std::vector<uint32>::size_type index)
{
    const OggPage &page = m_pages[m_page];
    m_offset = page.dataOffset(m_segment = index);
}

/*!
 * \brief Returns the index of the current segment (in the current page) if the iterator is valid; otherwise an undefined index is returned.
 */
inline std::vector<uint32>::size_type OggIterator::currentSegmentIndex() const
{
    return m_segment;
}

/*!
 * \brief Returns the start offset of the current segment in the input stream if the iterator is valid; otherwise an undefined offset is returned.
 * \sa currentCharacterOffset()
 */
inline uint64 OggIterator::currentSegmentOffset() const
{
    return m_offset;
}

/*!
 * \brief Returns the offset of the current character in the input stream if the iterator is valid; otherwise an undefined offset is returned.
 * \sa currentSegmentOffset()
 */
inline uint64 OggIterator::currentCharacterOffset() const
{
    return m_offset + m_bytesRead;
}

/*!
 * \brief Returns the size of the current segment.
 *
 * This method should never be called on an invalid iterator, since this causes undefined behaviour.
 */
inline uint32 OggIterator::currentSegmentSize() const
{
    return m_pages[m_page].segmentSizes()[m_segment];
}

/*!
 * \brief Allows to filter pages by the specified \a streamSerialId.
 *
 * Pages which do not match the specified \a streamSerialId will be skipped when getting the previous or
 * the next page.
 *
 * \sa removeFilter()
 */
inline void OggIterator::setFilter(uint32 streamSerialId)
{
    m_hasIdFilter = true;
    m_idFilter = streamSerialId;
}

/*!
 * \brief Removes a previously set filter.
 * \sa setFilter()
 */
inline void OggIterator::removeFilter()
{
    m_hasIdFilter = false;
}

/*!
 * \brief Returns an indication whether all pages have been fetched.
 *
 * This means that for each page in the stream in the specified range (stream and range have been specified when
 * constructing the iterator) an OggPage instance has been created and pushed to pages(). This is independend from
 * the current iterator position. Fetched pages remain after resetting the iterator.
 */
inline bool OggIterator::areAllPagesFetched() const
{
    return (m_pages.empty() ? m_startOffset : m_pages.back().startOffset() + m_pages.back().totalSize()) >= m_streamSize;
}

/*!
 * \brief Increments the current position by one segment if the iterator is valid; otherwise nothing happens.
 */
inline OggIterator &OggIterator::operator ++()
{
    nextSegment();
    return *this;
}

/*!
 * \brief Increments the current position by one segment if the iterator is valid; otherwise nothing happens.
 */
inline OggIterator OggIterator::operator ++(int)
{
    OggIterator tmp = *this;
    nextSegment();
    return tmp;
}

/*!
 * \brief Decrements the current position by one segment if the iterator is valid; otherwise nothing happens.
 */
inline OggIterator &OggIterator::operator --()
{
    previousSegment();
    return *this;
}

/*!
 * \brief Decrements the current position by one segment if the iterator is valid; otherwise nothing happens.
 */
inline OggIterator OggIterator::operator --(int)
{
    OggIterator tmp = *this;
    previousSegment();
    return tmp;
}

/*!
 * \brief Returns whether the specified \a page matches the current filter.
 */
inline bool OggIterator::matchesFilter(const OggPage &page)
{
    return !m_hasIdFilter || m_idFilter == page.streamSerialNumber();
}

}

#endif // MEDIA_OGGITERATOR_H