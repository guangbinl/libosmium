#ifndef OSMIUM_IO_DETAIL_PBF_INPUT_FORMAT_HPP
#define OSMIUM_IO_DETAIL_PBF_INPUT_FORMAT_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013,2014 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <ratio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/io/detail/input_format.hpp>
#include <osmium/io/detail/pbf.hpp> // IWYU pragma: export
#include <osmium/io/detail/zlib.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/io/header.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/timestamp.hpp>
#include <osmium/thread/name.hpp>
#include <osmium/thread/pool.hpp>
#include <osmium/thread/queue.hpp>
#include <osmium/util/cast.hpp>

namespace osmium {

    /**
     * Exception thrown when there was a problem with parsing the PBF format of
     * a file.
     */
    struct pbf_error : public io_error {

        pbf_error(const std::string& what) :
            io_error(std::string("PBF error: ") + what) {
        }

        pbf_error(const char* what) :
            io_error(std::string("PBF error: ") + what) {
        }

    }; // struct pbf_error

    namespace io {

        class File;

        namespace detail {

            class PBFPrimitiveBlockParser {

                static constexpr size_t initial_buffer_size = 2 * 1024 * 1024;

                const void* m_data;
                const int m_size;

                const OSMPBF::StringTable* m_stringtable;
                int64_t m_lon_offset;
                int64_t m_lat_offset;
                int64_t m_date_factor;
                int32_t m_granularity;

                osmium::osm_entity_bits::type m_read_types;

                osmium::memory::Buffer m_buffer;

                PBFPrimitiveBlockParser(const PBFPrimitiveBlockParser&) = delete;
                PBFPrimitiveBlockParser(PBFPrimitiveBlockParser&&) = delete;

                PBFPrimitiveBlockParser& operator=(const PBFPrimitiveBlockParser&) = delete;
                PBFPrimitiveBlockParser& operator=(PBFPrimitiveBlockParser&&) = delete;

            public:

                explicit PBFPrimitiveBlockParser(const void* data, const int size, osmium::osm_entity_bits::type read_types) :
                    m_data(data),
                    m_size(size),
                    m_stringtable(nullptr),
                    m_lon_offset(0),
                    m_lat_offset(0),
                    m_date_factor(1000),
                    m_granularity(100),
                    m_read_types(read_types),
                    m_buffer(initial_buffer_size) {
                }

                ~PBFPrimitiveBlockParser() = default;

                osmium::memory::Buffer operator()() {
                    OSMPBF::PrimitiveBlock pbf_primitive_block;
                    if (!pbf_primitive_block.ParseFromArray(m_data, m_size)) {
                        throw osmium::pbf_error("failed to parse PrimitiveBlock");
                    }

                    m_stringtable = &pbf_primitive_block.stringtable();
                    m_lon_offset  = pbf_primitive_block.lon_offset();
                    m_lat_offset  = pbf_primitive_block.lat_offset();
                    m_date_factor = pbf_primitive_block.date_granularity() / 1000;
                    m_granularity = pbf_primitive_block.granularity();

                    for (int i=0; i < pbf_primitive_block.primitivegroup_size(); ++i) {
                        const OSMPBF::PrimitiveGroup& group = pbf_primitive_block.primitivegroup(i);

                        if (group.has_dense())  {
                            if (m_read_types & osmium::osm_entity_bits::node) parse_dense_node_group(group);
                        } else if (group.ways_size() != 0) {
                            if (m_read_types & osmium::osm_entity_bits::way) parse_way_group(group);
                        } else if (group.relations_size() != 0) {
                            if (m_read_types & osmium::osm_entity_bits::relation) parse_relation_group(group);
                        } else if (group.nodes_size() != 0) {
                            if (m_read_types & osmium::osm_entity_bits::node) parse_node_group(group);
                        } else {
                            throw osmium::pbf_error("group of unknown type");
                        }
                    }

                    return std::move(m_buffer);
                }

            private:

                template <class TBuilder, class TPBFObject>
                void parse_attributes(TBuilder& builder, const TPBFObject& pbf_object) {
                    auto& object = builder.object();

                    object.set_id(pbf_object.id());

                    if (pbf_object.has_info()) {
                        object.set_version(static_cast_with_assert<object_version_type>(pbf_object.info().version()))
                            .set_changeset(static_cast_with_assert<changeset_id_type>(pbf_object.info().changeset()))
                            .set_timestamp(pbf_object.info().timestamp() * m_date_factor)
                            .set_uid_from_signed(pbf_object.info().uid());
                        if (pbf_object.info().has_visible()) {
                            object.set_visible(pbf_object.info().visible());
                        }
                        builder.add_user(m_stringtable->s(static_cast_with_assert<int>(pbf_object.info().user_sid())));
                    } else {
                        builder.add_user("", 1);
                    }
                }

                void parse_node_group(const OSMPBF::PrimitiveGroup& group) {
                    for (int i=0; i < group.nodes_size(); ++i) {
                        osmium::builder::NodeBuilder builder(m_buffer);
                        const OSMPBF::Node& pbf_node = group.nodes(i);
                        parse_attributes(builder, pbf_node);

                        if (builder.object().visible()) {
                            builder.object().set_location(osmium::Location(
                                              (pbf_node.lon() * m_granularity + m_lon_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision),
                                              (pbf_node.lat() * m_granularity + m_lat_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision)));
                        }

                        if (pbf_node.keys_size() > 0) {
                            osmium::builder::TagListBuilder tl_builder(m_buffer, &builder);
                            for (int tag=0; tag < pbf_node.keys_size(); ++tag) {
                                tl_builder.add_tag(m_stringtable->s(static_cast<int>(pbf_node.keys(tag))),
                                                   m_stringtable->s(static_cast<int>(pbf_node.vals(tag))));
                            }
                        }

                        m_buffer.commit();
                    }
                }

                void parse_way_group(const OSMPBF::PrimitiveGroup& group) {
                    for (int i=0; i < group.ways_size(); ++i) {
                        osmium::builder::WayBuilder builder(m_buffer);
                        const OSMPBF::Way& pbf_way = group.ways(i);
                        parse_attributes(builder, pbf_way);

                        if (pbf_way.refs_size() > 0) {
                            osmium::builder::WayNodeListBuilder wnl_builder(m_buffer, &builder);
                            int64_t ref = 0;
                            for (int n=0; n < pbf_way.refs_size(); ++n) {
                                ref += pbf_way.refs(n);
                                wnl_builder.add_node_ref(ref);
                            }
                        }

                        if (pbf_way.keys_size() > 0) {
                            osmium::builder::TagListBuilder tl_builder(m_buffer, &builder);
                            for (int tag=0; tag < pbf_way.keys_size(); ++tag) {
                                tl_builder.add_tag(m_stringtable->s(static_cast<int>(pbf_way.keys(tag))),
                                                   m_stringtable->s(static_cast<int>(pbf_way.vals(tag))));
                            }
                        }

                        m_buffer.commit();
                    }
                }

                void parse_relation_group(const OSMPBF::PrimitiveGroup& group) {
                    for (int i=0; i < group.relations_size(); ++i) {
                        osmium::builder::RelationBuilder builder(m_buffer);
                        const OSMPBF::Relation& pbf_relation = group.relations(i);
                        parse_attributes(builder, pbf_relation);

                        if (pbf_relation.types_size() > 0) {
                            osmium::builder::RelationMemberListBuilder rml_builder(m_buffer, &builder);
                            int64_t ref = 0;
                            for (int n=0; n < pbf_relation.types_size(); ++n) {
                                ref += pbf_relation.memids(n);
                                rml_builder.add_member(osmpbf_membertype_to_item_type(pbf_relation.types(n)), ref, m_stringtable->s(pbf_relation.roles_sid(n)));
                            }
                        }

                        if (pbf_relation.keys_size() > 0) {
                            osmium::builder::TagListBuilder tl_builder(m_buffer, &builder);
                            for (int tag=0; tag < pbf_relation.keys_size(); ++tag) {
                                tl_builder.add_tag(m_stringtable->s(static_cast<int>(pbf_relation.keys(tag))),
                                                   m_stringtable->s(static_cast<int>(pbf_relation.vals(tag))));
                            }
                        }

                        m_buffer.commit();
                    }
                }

                int add_tags(const OSMPBF::DenseNodes& dense, int n, osmium::builder::NodeBuilder* builder) {
                    if (n >= dense.keys_vals_size()) {
                        return n;
                    }

                    if (dense.keys_vals(n) == 0) {
                        return n+1;
                    }

                    osmium::builder::TagListBuilder tl_builder(m_buffer, builder);

                    while (n < dense.keys_vals_size()) {
                        int tag_key_pos = dense.keys_vals(n++);

                        if (tag_key_pos == 0) {
                            break;
                        }

                        tl_builder.add_tag(m_stringtable->s(tag_key_pos),
                                           m_stringtable->s(dense.keys_vals(n)));

                        ++n;
                    }

                    return n;
                }

                void parse_dense_node_group(const OSMPBF::PrimitiveGroup& group) {
                    int64_t last_dense_id        = 0;
                    int64_t last_dense_latitude  = 0;
                    int64_t last_dense_longitude = 0;
                    int64_t last_dense_uid       = 0;
                    int64_t last_dense_user_sid  = 0;
                    int64_t last_dense_changeset = 0;
                    int64_t last_dense_timestamp = 0;
                    int     last_dense_tag       = 0;

                    const OSMPBF::DenseNodes& dense = group.dense();

                    for (int i=0; i < dense.id_size(); ++i) {
                        bool visible = true;

                        last_dense_id        += dense.id(i);
                        last_dense_latitude  += dense.lat(i);
                        last_dense_longitude += dense.lon(i);

                        if (dense.has_denseinfo()) {
                            last_dense_changeset += dense.denseinfo().changeset(i);
                            last_dense_timestamp += dense.denseinfo().timestamp(i);
                            last_dense_uid       += dense.denseinfo().uid(i);
                            last_dense_user_sid  += dense.denseinfo().user_sid(i);
                            if (dense.denseinfo().visible_size() > 0) {
                                visible = dense.denseinfo().visible(i);
                            }
                            assert(last_dense_changeset >= 0);
                            assert(last_dense_timestamp >= 0);
                            assert(last_dense_uid >= -1);
                            assert(last_dense_user_sid >= 0);
                        }

                        osmium::builder::NodeBuilder builder(m_buffer);
                        osmium::Node& node = builder.object();

                        node.set_id(last_dense_id);

                        if (dense.has_denseinfo()) {
                            auto v = dense.denseinfo().version(i);
                            assert(v > 0);
                            node.set_version(static_cast<osmium::object_version_type>(v));
                            node.set_changeset(static_cast<osmium::changeset_id_type>(last_dense_changeset));
                            node.set_timestamp(last_dense_timestamp * m_date_factor);
                            node.set_uid_from_signed(static_cast<osmium::signed_user_id_type>(last_dense_uid));
                            node.set_visible(visible);
                            builder.add_user(m_stringtable->s(static_cast<int>(last_dense_user_sid)));
                        } else {
                            builder.add_user("", 1);
                        }

                        if (visible) {
                            builder.object().set_location(osmium::Location(
                                              (last_dense_longitude * m_granularity + m_lon_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision),
                                              (last_dense_latitude  * m_granularity + m_lat_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision)));
                        }

                        last_dense_tag = add_tags(dense, last_dense_tag, &builder);
                        m_buffer.commit();
                    }
                }

            }; // class PBFPrimitiveBlockParser

            typedef osmium::thread::Queue<std::future<osmium::memory::Buffer>> queue_type;

            class InputQueueReader {

                osmium::thread::Queue<std::string>& m_queue;
                std::string m_buffer;

            public:

                InputQueueReader(osmium::thread::Queue<std::string>& queue) :
                    m_queue(queue) {
                }

                bool operator()(unsigned char* data, size_t size) {
                    while (m_buffer.size() < size) {
                        std::string new_data;
                        m_queue.wait_and_pop(new_data);
                        if (new_data.empty()) {
                            return false;
                        }
                        m_buffer += new_data;
                    }
                    memcpy(data, m_buffer.data(), size);
                    m_buffer.erase(0, size);
                    return true;
                }

            }; // class InputQueueReader

            template <class TDerived>
            class BlobParser {

            protected:

                std::shared_ptr<unsigned char> m_input_buffer;
                const int m_size;
                const int m_blob_num;
                InputQueueReader& m_input_queue_reader;

                BlobParser(const int size, const int blob_num, InputQueueReader& input_queue_reader) :
                    m_input_buffer(new unsigned char[size], [](unsigned char* ptr) { delete[] ptr; }),
                    m_size(size),
                    m_blob_num(blob_num),
                    m_input_queue_reader(input_queue_reader) {
                    if (size < 0 || size > OSMPBF::max_uncompressed_blob_size) {
                        throw osmium::pbf_error(std::string("invalid blob size: " + std::to_string(size)));
                    }
                    if (! input_queue_reader(m_input_buffer.get(), static_cast<size_t>(size))) {
                        throw osmium::pbf_error("truncated data (EOF encountered)");
                    }
                }

            public:

                void doit() {
                    OSMPBF::Blob pbf_blob;
                    if (!pbf_blob.ParseFromArray(m_input_buffer.get(), m_size)) {
                        throw osmium::pbf_error("failed to parse blob");
                    }

                    if (pbf_blob.has_raw()) {
                        static_cast<TDerived*>(this)->handle_blob(pbf_blob.raw());
                        return;
                    } else if (pbf_blob.has_zlib_data()) {
                        auto raw_size = pbf_blob.raw_size();
                        assert(raw_size >= 0);
                        assert(raw_size <= OSMPBF::max_uncompressed_blob_size);

                        std::string unpack_buffer { osmium::io::detail::zlib_uncompress(pbf_blob.zlib_data(), static_cast<unsigned long>(raw_size)) };
                        static_cast<TDerived*>(this)->handle_blob(unpack_buffer);
                        return;
                    } else if (pbf_blob.has_lzma_data()) {
                        throw osmium::pbf_error("lzma blobs not implemented");
                    } else {
                        throw osmium::pbf_error("blob contains no data");
                    }
                }

                osmium::memory::Buffer operator()() {
                    OSMPBF::Blob pbf_blob;
                    if (!pbf_blob.ParseFromArray(m_input_buffer.get(), m_size)) {
                        throw osmium::pbf_error("failed to parse blob");
                    }

                    if (pbf_blob.has_raw()) {
                        return static_cast<TDerived*>(this)->handle_blob(pbf_blob.raw());
                    } else if (pbf_blob.has_zlib_data()) {
                        auto raw_size = pbf_blob.raw_size();
                        assert(raw_size >= 0);
                        assert(raw_size <= OSMPBF::max_uncompressed_blob_size);

                        std::string unpack_buffer { osmium::io::detail::zlib_uncompress(pbf_blob.zlib_data(), static_cast<unsigned long>(raw_size)) };
                        return static_cast<TDerived*>(this)->handle_blob(unpack_buffer);
                    } else if (pbf_blob.has_lzma_data()) {
                        throw osmium::pbf_error("lzma blobs not implemented");
                    } else {
                        throw osmium::pbf_error("blob contains no data");
                    }
                }

            }; // class BlobParser;

            class HeaderBlobParser : public BlobParser<HeaderBlobParser> {

                osmium::io::Header& m_header;

                void handle_blob(const std::string& data) {
                    OSMPBF::HeaderBlock pbf_header_block;
                    if (!pbf_header_block.ParseFromArray(data.data(), static_cast_with_assert<int>(data.size()))) {
                        throw osmium::pbf_error("failed to parse HeaderBlock");
                    }

                    for (int i=0; i < pbf_header_block.required_features_size(); ++i) {
                        const std::string& feature = pbf_header_block.required_features(i);

                        if (feature == "OsmSchema-V0.6") continue;
                        if (feature == "DenseNodes") {
                            m_header.set("pbf_dense_nodes", true);
                            continue;
                        }
                        if (feature == "HistoricalInformation") {
                            m_header.set_has_multiple_object_versions(true);
                            continue;
                        }

                        throw osmium::pbf_error(std::string("required feature not supported: ") + feature);
                    }

                    for (int i=0; i < pbf_header_block.optional_features_size(); ++i) {
                        const std::string& feature = pbf_header_block.optional_features(i);
                        m_header.set("pbf_optional_feature_" + std::to_string(i), feature);
                    }

                    if (pbf_header_block.has_writingprogram()) {
                        m_header.set("generator", pbf_header_block.writingprogram());
                    }

                    if (pbf_header_block.has_bbox()) {
                        const OSMPBF::HeaderBBox& pbf_bbox = pbf_header_block.bbox();
                        const int64_t resolution_convert = OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision;
                        osmium::Box box;
                        box.extend(osmium::Location(pbf_bbox.left()  / resolution_convert, pbf_bbox.bottom() / resolution_convert));
                        box.extend(osmium::Location(pbf_bbox.right() / resolution_convert, pbf_bbox.top()    / resolution_convert));
                        m_header.add_box(box);
                    }

                    if (pbf_header_block.has_osmosis_replication_timestamp()) {
                        m_header.set("osmosis_replication_timestamp", osmium::Timestamp(pbf_header_block.osmosis_replication_timestamp()).to_iso());
                    }

                    if (pbf_header_block.has_osmosis_replication_sequence_number()) {
                        m_header.set("osmosis_replication_sequence_number", std::to_string(pbf_header_block.osmosis_replication_sequence_number()));
                    }

                    if (pbf_header_block.has_osmosis_replication_base_url()) {
                        m_header.set("osmosis_replication_base_url", pbf_header_block.osmosis_replication_base_url());
                    }
                }

            public:

                friend class BlobParser<HeaderBlobParser>;

                HeaderBlobParser(const int size, InputQueueReader& input_queue_reader, osmium::io::Header& header) :
                    BlobParser(size, 0, input_queue_reader),
                    m_header(header) {
                }

            }; // class HeaderBlobParser

            class DataBlobParser : public BlobParser<DataBlobParser> {

                osmium::osm_entity_bits::type m_read_types;

                osmium::memory::Buffer handle_blob(const std::string& data) {
                    PBFPrimitiveBlockParser parser(data.data(), static_cast_with_assert<int>(data.size()), m_read_types);
                    return std::move(parser());
                }

            public:

                friend class BlobParser<DataBlobParser>;

                DataBlobParser(const int size, const int blob_num, InputQueueReader& input_queue_reader, osmium::osm_entity_bits::type read_types) :
                    BlobParser(size, blob_num, input_queue_reader),
                    m_read_types(read_types) {
                }

            }; // class DataBlobParser

            /**
             * Class for parsing PBF files.
             */
            class PBFInputFormat : public osmium::io::detail::InputFormat {

                bool m_use_thread_pool;
                queue_type m_queue;
                const size_t m_max_work_queue_size;
                const size_t m_max_buffer_queue_size;
                std::atomic<bool> m_done;
                std::thread m_reader;
                OSMPBF::BlobHeader m_blob_header;
                InputQueueReader m_input_queue_reader;

                /**
                 * Read BlobHeader by first reading the size and then the BlobHeader.
                 * The BlobHeader contains a type field (which is checked against
                 * the expected type) and a size field.
                 *
                 * @param expected_type Expected type of data ("OSMHeader" or "OSMData").
                 * @returns Size of the data read from BlobHeader (0 on EOF).
                 */
                int read_blob_header(const char* expected_type) {
                    uint32_t size_in_network_byte_order;

                    if (! m_input_queue_reader(reinterpret_cast<unsigned char*>(&size_in_network_byte_order), sizeof(size_in_network_byte_order))) {
                        return 0; // EOF
                    }

                    uint32_t size = ntohl(size_in_network_byte_order);
                    if (size > static_cast<uint32_t>(OSMPBF::max_blob_header_size)) {
                        throw osmium::pbf_error("invalid BlobHeader size (> max_blob_header_size)");
                    }

                    unsigned char blob_header_buffer[OSMPBF::max_blob_header_size];
                    if (! m_input_queue_reader(blob_header_buffer, size)) {
                        throw osmium::pbf_error("read error");
                    }

                    if (!m_blob_header.ParseFromArray(blob_header_buffer, static_cast<int>(size))) {
                        throw osmium::pbf_error("failed to parse BlobHeader");
                    }

                    if (std::strcmp(m_blob_header.type().c_str(), expected_type)) {
                        throw osmium::pbf_error("blob does not have expected type (OSMHeader in first blob, OSMData in following blobs)");
                    }

                    return m_blob_header.datasize();
                }

                void parse_osm_data(osmium::osm_entity_bits::type read_types) {
                    osmium::thread::set_thread_name("_osmium_pbf_in");
                    int n=0;
                    while (int size = read_blob_header("OSMData")) {
                        DataBlobParser data_blob_parser(size, n, m_input_queue_reader, read_types);

                        if (m_use_thread_pool) {
                            m_queue.push(osmium::thread::Pool::instance().submit(data_blob_parser));

                            // if the work queue is getting too large, wait for a while
                            while (!m_done && osmium::thread::Pool::instance().queue_size() >= m_max_work_queue_size) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }
                        } else {
                            std::promise<osmium::memory::Buffer> promise;
                            m_queue.push(promise.get_future());
                            promise.set_value(data_blob_parser());
                        }
                        ++n;

                        // wait if the backlog of buffers with parsed data is too large
                        while (!m_done && m_queue.size() > m_max_buffer_queue_size) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }

                        if (m_done) {
                            return;
                        }
                    }
                    m_done = true;
                }

            public:

                /**
                 * Instantiate PBF Parser
                 *
                 * @param file osmium::io::File instance describing file to be read from.
                 * @param read_which_entities Which types of OSM entities (nodes, ways, relations, changesets) should be parsed?
                 * @param input_queue String queue where data is read from.
                 */
                PBFInputFormat(const osmium::io::File& file, osmium::osm_entity_bits::type read_which_entities, osmium::thread::Queue<std::string>& input_queue) :
                    osmium::io::detail::InputFormat(file, read_which_entities, input_queue),
                    m_use_thread_pool(true),
                    m_queue(),
                    m_max_work_queue_size(10), // XXX tune these settings
                    m_max_buffer_queue_size(20), // XXX tune these settings
                    m_done(false),
                    m_input_queue_reader(input_queue) {
                    GOOGLE_PROTOBUF_VERIFY_VERSION;

                    // handle OSMHeader
                    int size = read_blob_header("OSMHeader");

                    {
                        HeaderBlobParser header_blob_parser(size, m_input_queue_reader, m_header);
                        header_blob_parser.doit();
                    }

                    if (m_read_which_entities != osmium::osm_entity_bits::nothing) {
                        m_reader = std::thread(&PBFInputFormat::parse_osm_data, this, m_read_which_entities);
                    }
                }

                ~PBFInputFormat() {
                    m_done = true;
                    if (m_reader.joinable()) {
                        m_reader.join();
                    }
                }

                /**
                 * Returns the next buffer with OSM data read from the PBF file.
                 * Blocks if data is not available yet.
                 * Returns an empty buffer at end of input.
                 */
                osmium::memory::Buffer read() override {
                    if (!m_done || !m_queue.empty()) {
                        std::future<osmium::memory::Buffer> buffer_future;
                        m_queue.wait_and_pop(buffer_future);
                        return std::move(buffer_future.get());
                    }

                    return osmium::memory::Buffer();
                }

            }; // class PBFInputFormat

            namespace {

                const bool registered_pbf_input = osmium::io::detail::InputFormatFactory::instance().register_input_format(osmium::io::file_format::pbf,
                    [](const osmium::io::File& file, osmium::osm_entity_bits::type read_which_entities, osmium::thread::Queue<std::string>& input_queue) {
                        return new osmium::io::detail::PBFInputFormat(file, read_which_entities, input_queue);
                });

            } // anonymous namespace

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_DETAIL_PBF_INPUT_FORMAT_HPP
