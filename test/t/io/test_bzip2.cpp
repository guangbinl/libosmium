#include "catch.hpp"

#include "utils.hpp"

#include <osmium/io/bzip2_compression.hpp>
#include <osmium/io/detail/read_write.hpp>

#include <string>

TEST_CASE("Invalid file descriptor of bzip2-compressed file") {
    REQUIRE_THROWS_AS(osmium::io::Bzip2Decompressor{-1}, const std::system_error&);
}

TEST_CASE("Non-open file descriptor of bzip2-compressed file") {
    // 12345 is just a random file descriptor that should not be open
    REQUIRE_THROWS_AS(osmium::io::Bzip2Decompressor{12345}, const std::system_error&);
}

TEST_CASE("Empty bzip2-compressed file") {
    const int count1 = count_fds();

    const std::string input_file = with_data_dir("t/io/empty_file");
    const int fd = osmium::io::detail::open_for_reading(input_file);
    REQUIRE(fd > 0);

    const int count2 = count_fds();
    osmium::io::Bzip2Decompressor decomp{fd};
    REQUIRE_THROWS_AS(decomp.read(), const osmium::bzip2_error&);
    decomp.close();
    REQUIRE(count2 == count_fds());

    close(fd);
    REQUIRE(count1 == count_fds());
}

TEST_CASE("Read bzip2-compressed file") {
    const int count1 = count_fds();

    const std::string input_file = with_data_dir("t/io/data_bzip2.txt.bz2");
    const int fd = osmium::io::detail::open_for_reading(input_file);
    REQUIRE(fd > 0);

    size_t size = 0;
    std::string all;
    {
        const int count2 = count_fds();
        osmium::io::Bzip2Decompressor decomp{fd};
        for (std::string data = decomp.read(); !data.empty(); data = decomp.read()) {
            size += data.size();
            all += data;
        }
        decomp.close();
        REQUIRE(count2 == count_fds());
    }

    REQUIRE(size >= 9);
    all.resize(8);
    REQUIRE("TESTDATA" == all);

    close(fd);
    REQUIRE(count1 == count_fds());
}

TEST_CASE("Read bzip2-compressed file without explicit close") {
    const int count1 = count_fds();

    const std::string input_file = with_data_dir("t/io/data_bzip2.txt.bz2");
    const int fd = osmium::io::detail::open_for_reading(input_file);
    REQUIRE(fd > 0);

    size_t size = 0;
    std::string all;
    const int count2 = count_fds();
    {
        osmium::io::Bzip2Decompressor decomp{fd};
        for (std::string data = decomp.read(); !data.empty(); data = decomp.read()) {
            size += data.size();
            all += data;
        }
    }
    REQUIRE(count2 == count_fds());

    REQUIRE(size >= 9);
    all.resize(8);
    REQUIRE("TESTDATA" == all);

    close(fd);
    REQUIRE(count1 == count_fds());
}

TEST_CASE("Corrupted bzip2-compressed file") {
    const int count1 = count_fds();

    const std::string input_file = with_data_dir("t/io/corrupt_data_bzip2.txt.bz2");
    const int fd = osmium::io::detail::open_for_reading(input_file);
    REQUIRE(fd > 0);

    const int count2 = count_fds();

    osmium::io::Bzip2Decompressor decomp{fd};
    REQUIRE_THROWS_AS(decomp.read(), const osmium::bzip2_error&);
    decomp.close();

    REQUIRE(count2 == count_fds());

    close(fd);
    REQUIRE(count1 == count_fds());
}

