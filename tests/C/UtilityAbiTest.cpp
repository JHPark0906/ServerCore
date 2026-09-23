#include "AbiSocketTestSupport.h"
#include "ConfigTestSupport.h"
#include "ServerCore/C/BinaryIO.h"
#include "ServerCore/C/Files.h"
namespace
{
using namespace AbiTest;
using ServerCoreTest::ExpectTrue;
void UtilityOwnedHandles()
{
    sc_binary_writer* writerRaw = nullptr;
    ExpectTrue(
        sc_binary_writer_create(64, SC_BIG_ENDIAN, &writerRaw) == SC_OK, "C binary writer creates");
    Handle<sc_binary_writer, sc_binary_writer_destroy> writer(writerRaw, sc_binary_writer_destroy);
    ExpectTrue(sc_binary_write_unsigned(writer.get(), 0x12345678, 4) == SC_OK &&
                   sc_binary_write_utf8(writer.get(), Bytes("owned"), 5) == SC_OK,
        "C integer and text encoding");
    sc_bytes encoded{};
    sc_binary_writer_view(writer.get(), &encoded);
    sc_binary_reader* readerRaw = nullptr;
    ExpectTrue(sc_binary_reader_create(encoded, SC_BIG_ENDIAN, 64, &readerRaw) == SC_OK,
        "C reader copies input");
    Handle<sc_binary_reader, sc_binary_reader_destroy> reader(readerRaw, sc_binary_reader_destroy);
    writer.reset();
    uint64_t value = 99;
    ExpectTrue(sc_binary_read_unsigned(reader.get(), 3, &value) == SC_INVALID_ARGUMENT &&
                   value == 99 && sc_binary_reader_position(reader.get()) == 0,
        "C errors preserve outputs and cursor");
    ExpectTrue(sc_binary_read_unsigned(reader.get(), 4, &value) == SC_OK && value == 0x12345678,
        "reader outlives writer bytes");
    sc_bytes view{};
    ExpectTrue(sc_binary_read_utf8(reader.get(), 5, &view) == SC_OK && Text(view) == "owned",
        "reader owned view");
    sc_protocol_offer local{ 2, 0, 3 }, remote{ 2, 0, 1 }, selected{};
    ExpectTrue(sc_protocol_negotiate(&local, 1, &remote, 1, 1, &selected) == SC_OK &&
                   selected.version == 2 && selected.features == 1,
        "C explicit protocol selection");
    ServerCoreTest::ScopedConfigFile target("old");
    sc_atomic_file_options options{};
    sc_atomic_file_options_init(&options, sizeof(options));
    sc_atomic_file* fileRaw = nullptr;
    ExpectTrue(sc_atomic_file_create(Bytes(target.Path()), &options, &fileRaw) == SC_OK,
        "C file creates temporary");
    Handle<sc_atomic_file, sc_atomic_file_destroy> file(fileRaw, sc_atomic_file_destroy);
    ExpectTrue(sc_atomic_file_write(file.get(), Bytes("new")) == SC_OK &&
                   sc_atomic_file_written(file.get()) == 3 &&
                   sc_atomic_file_commit(file.get()) == SC_OK &&
                   sc_atomic_file_committed(file.get()),
        "C file publishes and reports commit state");
    file.reset();
    std::ifstream input(std::filesystem::path(std::string(target.Path())), std::ios::binary);
    std::string actual{ std::istreambuf_iterator<char>(input), {} };
    ExpectTrue(actual == "new", "C file result survives owner release");
}
ServerCoreTest::CheckRegistration a("CAbi.UtilityOwnedHandles", UtilityOwnedHandles);
}
