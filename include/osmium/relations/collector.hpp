#ifndef OSMIUM_RELATIONS_COLLECTOR_HPP
#define OSMIUM_RELATIONS_COLLECTOR_HPP

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
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <vector>

#include <osmium/osm/item_type.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/handler.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/visitor.hpp>

#include <osmium/relations/detail/relation_meta.hpp>
#include <osmium/relations/detail/member_meta.hpp>

namespace osmium {

    class Node;
    class Way;
    class Relation;
    class RelationMember;

    /**
     * @brief Namespace for code related to OSM relations
     */
    namespace relations {

        /**
         * The Collector class collects members of a relation. This is a generic
         * base class that can be used to assemble all kinds of relations. It has numerous
         * hooks you can implement in derived classes to customize its behaviour.
         *
         * The collector provides two handlers (HandlerPass1 and HandlerPass2) for a first
         * and second pass through an input file, respectively. In the first pass all
         * relations we are interested in are stored in RelationMeta objects in the
         * m_relations vector. All members we are interested in are stored in MemberMeta
         * objects in the m_member_meta vectors.
         * The MemberMeta objects also store the information where the relations containing
         * those members are to be found.
         *
         * Later the m_member_meta vectors are sorted according to the
         * member ids so that a binary search (with std::equal_range) can be used in the second
         * pass to find the parent relations for each node, way, or relation coming along.
         * The member objects are stored together with their relation and once a relation
         * is complete the complete_relation() method is called which you must overwrite in
         * a derived class of Collector.
         *
         * @tparam TCollector Derived class of this class.
         *
         * @tparam TNodes Are we interested in member nodes?
         *
         * @tparam TWays Are we interested in member ways?
         *
         * @tparam TRelations Are we interested in member relations?
         */
        template <class TCollector, bool TNodes, bool TWays, bool TRelations>
        class Collector {

            /**
             * This is the handler class for the first pass of the Collector.
             */
            class HandlerPass1 : public osmium::handler::Handler {

                TCollector& m_collector;

            public:

                HandlerPass1(TCollector& collector) :
                    m_collector(collector) {
                }

                void relation(const osmium::Relation& relation) {
                    if (m_collector.keep_relation(relation)) {
                        m_collector.add_relation(relation);
                    }
                }

            }; // class HandlerPass1

            /**
             * This is the handler class for the second pass of the Collector.
             */
            class HandlerPass2 : public osmium::handler::Handler {

                TCollector& m_collector;

                /**
                 * This variable is initialized with the number of different
                 * kinds of OSM objects we are interested in. If we only need
                 * way members (for instance for the multipolygon collector)
                 * it is intialized with 1 for instance. If node and way
                 * members are needed, it is initialized with 2.
                 *
                 * In the after_* methods of this handler, it is decremented
                 * and once it reaches 0, we know we have all members available
                 * that we are ever going to get.
                 */
                int m_want_types;

                /**
                 * Find this object in the member vectors and add it to all
                 * relations that need it.
                 *
                 * @returns true if the member was added to at least one
                 *          relation and false otherwise
                 */
                bool find_and_add_object(const osmium::Object& object) {
                    auto& mmv = m_collector.member_meta(object.type());
                    auto range = std::equal_range(mmv.begin(), mmv.end(), MemberMeta(object.id()));

//                    std::cerr << "looking for " << item_type_to_char(object.type()) << object.id() << " got range: " << std::distance(range.first, range.second) << "\n";

                    if (range.first == range.second) {
                        // nothing found
                        return false;
                    }

                    size_t pos = m_collector.members_buffer().committed();
                    m_collector.members_buffer().add_item(object);
                    m_collector.members_buffer().commit();

                    for (auto it = range.first; it != range.second; ++it) {
                        MemberMeta& member_meta = *it;
                        member_meta.buffer_offset(pos);
                        assert(member_meta.member_id() == object.id());
                        assert(member_meta.relation_pos() < m_collector.m_relations.size());
                        RelationMeta& relation_meta = m_collector.m_relations[member_meta.relation_pos()];
//                        std::cerr << "  => " << member_meta.member_pos() << " < " << m_collector.get_relation(relation_meta).members().size() << " (id=" << m_collector.get_relation(relation_meta).id() << ")\n";
                        assert(member_meta.member_pos() < m_collector.get_relation(relation_meta).members().size());
//                        std::cerr << "  add way " << member_meta.member_id() << " to rel " << m_collector.get_relation(relation_meta).id() << " at pos " << member_meta.member_pos() << "\n";
                        relation_meta.got_one_member();
                        if (relation_meta.has_all_members()) {
                            size_t pos = member_meta.relation_pos();
                            m_collector.complete_relation(relation_meta);
                            m_collector.m_relations[pos] = RelationMeta();
                            m_collector.possibly_purge_deleted_members();
                        }
                    }

                    return true;
                }

            public:

                HandlerPass2(TCollector& collector) :
                    m_collector(collector),
                    m_want_types((TNodes?1:0) + (TWays?1:0) + (TRelations?1:0)) {
                }

                void node(const osmium::Node& node) {
                    if (TNodes) {
                        if (! find_and_add_object(node)) {
                            m_collector.node_not_in_any_relation(node);
                        }
                    }
                }

                void way(const osmium::Way& way) {
                    if (TWays) {
                        if (! find_and_add_object(way)) {
                            m_collector.way_not_in_any_relation(way);
                        }
                    }
                }

                void relation(const osmium::Relation& relation) {
                    if (TRelations) {
                        if (! find_and_add_object(relation)) {
                            m_collector.relation_not_in_any_relation(relation);
                        }
                    }
                }

                void done() {
                    // clear all memory used by m_member_meta of this type
                    m_collector.member_meta(osmium::item_type::node).clear();
                    m_collector.member_meta(osmium::item_type::node).shrink_to_fit();
                    m_collector.member_meta(osmium::item_type::way).clear();
                    m_collector.member_meta(osmium::item_type::way).shrink_to_fit();
                    m_collector.member_meta(osmium::item_type::relation).clear();
                    m_collector.member_meta(osmium::item_type::relation).shrink_to_fit();
                    m_collector.done();
                }

            }; // class HandlerPass2

            HandlerPass2 m_handler_pass2;

            // All relations we are interested in will be kept in this buffer
            osmium::memory::Buffer m_relations_buffer;

            // All members we are interested in will be kept in this buffer
            osmium::memory::Buffer m_members_buffer;

            /// Vector with all relations we are interested in
            std::vector<RelationMeta> m_relations;

            /**
             * One vector each for nodes, ways, and relations containing all
             * mappings from member ids to their relations.
             */
            std::vector<MemberMeta> m_member_meta[3];

            int m_count_complete = 0;

            typedef std::function<void(const osmium::memory::Buffer&)> callback_func_type;
            callback_func_type m_callback;

        public:

            /**
             * Create an Collector.
             */
            Collector() :
                m_handler_pass2(*static_cast<TCollector*>(this)),
                m_relations_buffer(1024*1024, true),
                m_members_buffer(1024*1024, true),
                m_relations(),
                m_member_meta() {
            }

        protected:

            std::vector<MemberMeta>& member_meta(const item_type type) {
                return m_member_meta[static_cast<uint16_t>(type) - 1];
            }

            callback_func_type callback() {
                return m_callback;
            }

            const std::vector<RelationMeta>& relations() const {
                return m_relations;
            }

            /**
             * This method is called from the first pass handler for every
             * relation in the input, to check whether it should be kept.
             *
             * Overwrite this method in a child class to only add relations
             * you are interested in, for instance depending on the type tag.
             * Storing relations takes a lot of memory, so it makes sense to
             * filter this as much as possible.
             */
            bool keep_relation(const osmium::Relation& /*relation*/) const {
                return true;
            }

            /**
             * This method is called for every member of every relation that
             * should be kept. It should decide if the member is interesting or
             * not and return true or false to signal that. Only interesting
             * members are later added to the relation.
             *
             * Overwrite this method in a child class. In the MultiPolygonCollector
             * this is for instance used to only keep members of type way and
             * ignore all others.
             */
            bool keep_member(const osmium::relations::RelationMeta& /*relation_meta*/, const osmium::RelationMember& /*member*/) const {
                return true;
            }

            /**
             * This method is called for all nodes that are not a member of
             * any relation.
             *
             * Overwrite this method in a child class if you are interested
             * in this.
             */
            void node_not_in_any_relation(const osmium::Node& /*node*/) {
            }

            /**
             * This method is called for all ways that are not a member of
             * any relation.
             *
             * Overwrite this method in a child class if you are interested
             * in this.
             */
            void way_not_in_any_relation(const osmium::Way& /*way*/) {
            }

            /**
             * This method is called for all relations that are not a member of
             * any relation.
             *
             * Overwrite this method in a child class if you are interested
             * in this.
             */
            void relation_not_in_any_relation(const osmium::Relation& /*relation*/) {
            }

            /**
             * This method is called from the 2nd pass handler when all objects
             * of types we are interested in have been seen.
             *
             * Overwrite this method in a child class if you are interested
             * in this.
             *
             * Note that even after this call members might be missing if they
             * were not in the input file! The derived class has to handle this
             * case.
             */
            void done() {
            }

            /**
             * This removes all relations that have already been assembled
             * from the m_relations vector.
             */
            void clean_assembled_relations() {
                m_relations.erase(
                    std::remove_if(m_relations.begin(), m_relations.end(), has_all_members()),
                    m_relations.end()
                    );
            }

            const osmium::Relation& get_relation(size_t offset) const {
                return m_relations_buffer.get<osmium::Relation>(offset);
            }

            /**
             * Get the relation from a relation_meta.
             */
            const osmium::Relation& get_relation(const RelationMeta& relation_meta) const {
                return get_relation(relation_meta.relation_offset());
            }

            osmium::Object& get_member(size_t offset) const {
                return m_members_buffer.get<osmium::Object>(offset);
            }

            /**
             * Tell the Collector that you are interested in this relation
             * and want it kept until all members have been assembled and
             * it is handed back to you.
             *
             * The relation is copied and stored in a buffer inside the
             * collector.
             */
            void add_relation(const osmium::Relation& relation) {
                size_t offset = m_relations_buffer.committed();
                m_relations_buffer.add_item(relation);

                RelationMeta relation_meta(offset);

                int n=0;
                for (auto& member : m_relations_buffer.get<osmium::Relation>(offset).members()) {
                    if (static_cast<TCollector*>(this)->keep_member(relation_meta, member)) {
                        member_meta(member.type()).emplace_back(member.ref(), m_relations.size(), n);
                        relation_meta.increment_need_members();
                    } else {
                        member.ref(0); // set member id to zero to indicate we are not interested
                    }
                    ++n;
                }

                assert(offset == m_relations_buffer.committed());
                if (relation_meta.has_all_members()) {
                    m_relations_buffer.rollback();
                } else {
                    m_relations_buffer.commit();
                    m_relations.push_back(std::move(relation_meta));
//                    std::cerr << "added relation id=" << relation.id() << "\n";
                }
            }

            /**
             * Sort the vectors with the member infos so that we can do binary
             * search on them.
             */
            void sort_member_meta() {
                std::cerr << "relations:        " << m_relations.size() << "\n";
                std::cerr << "node members:     " << m_member_meta[0].size() << "\n";
                std::cerr << "way members:      " << m_member_meta[1].size() << "\n";
                std::cerr << "relation members: " << m_member_meta[2].size() << "\n";
                std::sort(m_member_meta[0].begin(), m_member_meta[0].end());
                std::sort(m_member_meta[1].begin(), m_member_meta[1].end());
                std::sort(m_member_meta[2].begin(), m_member_meta[2].end());
            }

        public:

            uint64_t used_memory() const {
                uint64_t nmembers = m_member_meta[0].capacity() + m_member_meta[1].capacity() + m_member_meta[2].capacity();
                uint64_t members = nmembers * sizeof(MemberMeta);
                uint64_t relations = m_relations.capacity() * sizeof(RelationMeta);
                uint64_t relations_buffer_capacity = m_relations_buffer.capacity();
                uint64_t members_buffer_capacity = m_members_buffer.capacity();

                std::cout << "  nR  = m_relations.capacity() ........... = " << std::setw(12) << m_relations.capacity() << "\n";
                std::cout << "  nMN = m_member_meta[NODE].capacity() ... = " << std::setw(12) << m_member_meta[0].capacity() << "\n";
                std::cout << "  nMW = m_member_meta[WAY].capacity() .... = " << std::setw(12) << m_member_meta[1].capacity() << "\n";
                std::cout << "  nMR = m_member_meta[RELATION].capacity() = " << std::setw(12) << m_member_meta[2].capacity() << "\n";
                std::cout << "  nM  = m_member_meta[*].capacity() ...... = " << std::setw(12) << nmembers << "\n";

                std::cout << "  sRM = sizeof(RelationMeta) ............. = " << std::setw(12) << sizeof(RelationMeta) << "\n";
                std::cout << "  sMM = sizeof(MemberMeta) ............... = " << std::setw(12) << sizeof(MemberMeta) << "\n\n";

                std::cout << "  nR * sRM ............................... = " << std::setw(12) << relations << "\n";
                std::cout << "  nM * sMM ............................... = " << std::setw(12) << members << "\n";
                std::cout << "  relations_buffer_capacity .............. = " << std::setw(12) << relations_buffer_capacity << "\n";
                std::cout << "  members_buffer_capacity ................ = " << std::setw(12) << members_buffer_capacity << "\n";

                uint64_t total = relations + members + relations_buffer_capacity + members_buffer_capacity;

                std::cout << "  total .................................. = " << std::setw(12) << total << "\n";
                std::cout << "  =======================================================\n";

                return relations_buffer_capacity + members_buffer_capacity + relations + members;
            }

            /**
             * Return reference to second pass handler.
             */
            HandlerPass2& handler(const callback_func_type& callback = nullptr) {
                m_callback = callback;
                return m_handler_pass2;
            }

            osmium::memory::Buffer& members_buffer() {
                return m_members_buffer;
            }

            size_t get_offset(osmium::item_type type, osmium::object_id_type id) {
                const auto& mmv = member_meta(type);
                const auto range = std::equal_range(mmv.cbegin(), mmv.cend(), MemberMeta(id));
                assert(range.first != range.second);
                return range.first->buffer_offset();
            }

            template <class TSource>
            void read_relations(TSource& source) {
                HandlerPass1 handler(*static_cast<TCollector*>(this));
                osmium::apply(source, handler);
                source.close();
                sort_member_meta();
            }

            void moving_in_buffer(size_t old_offset, size_t new_offset) {
                osmium::Object& object = m_members_buffer.get<osmium::Object>(old_offset);
                auto& mmv = m_member_meta[static_cast<uint16_t>(object.type()) - 1];
                auto range = std::equal_range(mmv.begin(), mmv.end(), osmium::relations::MemberMeta(object.id()));
                for (auto it = range.first; it != range.second; ++it) {
                    assert(it->buffer_offset() == old_offset);
                    it->buffer_offset(new_offset);
                }
            }

            /**
             * Decide whether to purge deleted members and then do it.
             *
             * Currently the purging is done every thousand calls.
             * This could probably be improved upon.
             */
            void possibly_purge_deleted_members() {
                ++m_count_complete;
                if (m_count_complete > 1000) { // XXX
                    std::cerr << "PURGE\n";
                    m_members_buffer.purge_deleted(this);
                    m_count_complete = 0;
                }
            }

        }; // class Collector

    } // namespace relations

} // namespace osmium

#endif // OSMIUM_RELATIONS_COLLECTOR_HPP
