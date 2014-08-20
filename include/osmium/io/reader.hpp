#ifndef OSMIUM_IO_READER_HPP
#define OSMIUM_IO_READER_HPP

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

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#ifndef _MSC_VER
#include <unistd.h>
#else
#endif
#include <utility>

#include <osmium/io/compression.hpp>
#include <osmium/io/detail/input_format.hpp>
#include <osmium/io/detail/read_thread.hpp>
#include <osmium/io/detail/read_write.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/header.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/thread/checked_task.hpp>
#include <osmium/thread/name.hpp>
#include <osmium/thread/queue.hpp>

namespace osmium {

    namespace io {

        /**
         * This is the user-facing interface for reading OSM files. Instantiate
         * an object of this class with a file name or osmium::io::File object
         * and then call read() on it in a loop until it returns an invalid
         * Buffer.
         */
        class Reader {

            osmium::io::File m_file;
            osmium::osm_entity_bits::type m_read_which_entities;
            std::atomic<bool> m_input_done;
            int m_childpid;

            osmium::thread::Queue<std::string> m_input_queue;

            std::unique_ptr<osmium::io::Decompressor> m_decompressor;
            osmium::thread::CheckedTask<detail::ReadThread> m_read_task;

            std::unique_ptr<osmium::io::detail::InputFormat> m_input;

            /**
             * Fork and execute the given command in the child.
             * A pipe is created between the child and the parent.
             * The child writes to the pipe, the parent reads from it.
             * This function never returns in the child.
             *
             * @param command Command to execute in the child.
             * @param filename Filename to give to command as argument.
             * @return File descriptor of pipe in the parent.
             * @throws std::system_error if a system call fails.
             */
            static int execute(const std::string& command, const std::string& filename, int* childpid) {
                int pipefd[2];
                if (pipe(pipefd) < 0) {
                    throw std::system_error(errno, std::system_category(), "opening pipe failed");
                }
                pid_t pid = fork();
                if (pid < 0) {
                    throw std::system_error(errno, std::system_category(), "fork failed");
                }
                if (pid == 0) { // child
                    // close all file descriptors except one end of the pipe
                    for (int i=0; i < 32; ++i) {
                        if (i != pipefd[1]) {
                            ::close(i);
                        }
                    }
                    if (dup2(pipefd[1], 1) < 0) { // put end of pipe as stdout/stdin
                        exit(1);
                    }

                    ::open("/dev/null", O_RDONLY); // stdin
                    ::open("/dev/null", O_WRONLY); // stderr
                    // hack: -g switches off globbing in curl which allows [] to be used in file names
                    // this is important for XAPI URLs
                    // in theory this execute() function could be used for other commands, but it is
                    // only used for curl at the moment, so this is okay.
                    if (::execlp(command.c_str(), command.c_str(), "-g", filename.c_str(), nullptr) < 0) {
                        exit(1);
                    }
                }
                // parent
                *childpid = pid;
                ::close(pipefd[1]);
                return pipefd[0];
            }

            /**
             * Open File for reading. Handles URLs or normal files. URLs
             * are opened by executing the "curl" program (which must be installed)
             * and reading from its output.
             *
             * @return File descriptor of open file or pipe.
             * @throws std::system_error if a system call fails.
             */
            static int open_input_file_or_url(const std::string& filename, int* childpid) {
                std::string protocol = filename.substr(0, filename.find_first_of(':'));
                if (protocol == "http" || protocol == "https" || protocol == "ftp" || protocol == "file") {
                    return execute("curl", filename, childpid);
                } else {
                    return osmium::io::detail::open_for_reading(filename);
                }
            }

        public:

            /**
             * Create new Reader object.
             *
             * @param file The file we want to open.
             * @param read_which_entities Which OSM entities (nodes, ways, relations, and/or changesets)
             *                            should be read from the input file. It can speed the read up
             *                            significantly if objects that are not needed anyway are not
             *                            parsed.
             */
            explicit Reader(const osmium::io::File& file, osmium::osm_entity_bits::type read_which_entities = osmium::osm_entity_bits::all) :
                m_file(file),
                m_read_which_entities(read_which_entities),
                m_input_done(false),
                m_childpid(0),
                m_input_queue(),
                m_decompressor(osmium::io::CompressionFactory::instance().create_decompressor(file.compression(), open_input_file_or_url(m_file.filename(), &m_childpid))),
                m_read_task(m_input_queue, m_decompressor.get(), m_input_done),
                m_input(osmium::io::detail::InputFormatFactory::instance().create_input(m_file, m_read_which_entities, m_input_queue)) {
            }

            explicit Reader(const std::string& filename, osmium::osm_entity_bits::type read_types = osmium::osm_entity_bits::all) :
                Reader(osmium::io::File(filename), read_types) {
            }

            explicit Reader(const char* filename, osmium::osm_entity_bits::type read_types = osmium::osm_entity_bits::all) :
                Reader(osmium::io::File(filename), read_types) {
            }

            Reader(const Reader&) = delete;
            Reader& operator=(const Reader&) = delete;

            ~Reader() {
                close();
            }

            /**
             * Close down the Reader. A call to this is optional, because the
             * destructor of Reader will also call this. But if you don't call
             * this function first, the destructor might throw an exception
             * which is not good.
             *
             * @throws Some form of std::runtime_error when there is a problem.
             */
            void close() {
                // Signal to input child process that it should wrap up.
                m_input_done = true;

                m_input->close();

                if (m_childpid) {
                    int status;
                    pid_t pid = ::waitpid(m_childpid, &status, 0);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
                    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        throw std::system_error(errno, std::system_category(), "subprocess returned error");
                    }
#pragma GCC diagnostic pop
                    m_childpid = 0;
                }

                m_read_task.close();
            }

            /**
             * Get the header data from the file.
             */
            osmium::io::Header header() const {
                return m_input->header();
            }

            /**
             * Reads the next buffer from the input. An invalid buffer signals
             * end-of-file. Do not call read() after the end-of-file.
             *
             * @returns Buffer.
             * @throws Some form of std::runtime_error if there is an error.
             */
            osmium::memory::Buffer read() {
                // If an exception happened in the input thread, re-throw
                // it in this (the main) thread.
                m_read_task.check_for_exception();

                if (m_read_which_entities == osmium::osm_entity_bits::nothing) {
                    // If the caller didn't want anything but the header, it will
                    // always get an empty buffer here.
                    return osmium::memory::Buffer();
                }
                return m_input->read();
            }

        }; // class Reader

        /**
         * Read contents of the given file into a buffer in one go. Takes
         * the same arguments as any of the Reader constructors.
         *
         * The buffer can take up quite a lot of memory, so don't do this
         * unless you are working with small OSM files and/or have lots of
         * RAM.
         */
        template <class... TArgs>
        osmium::memory::Buffer read_file(TArgs&&... args) {
            osmium::memory::Buffer buffer(1024*1024, osmium::memory::Buffer::auto_grow::yes);

            Reader reader(std::forward<TArgs>(args)...);
            while (osmium::memory::Buffer read_buffer = reader.read()) {
                buffer.add_buffer(read_buffer);
                buffer.commit();
            }

            return buffer;
        }

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_READER_HPP
