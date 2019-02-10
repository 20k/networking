#include "networking.hpp"
#include "serialisable.hpp"
#include <iostream>
#include <windows.h>
#include "netinterpolate.hpp"

void serialise_test()
{
    test_serialisable ser;
    ser.test_datamember = 5;

    nlohmann::json intermediate = serialise(ser);

    test_serialisable second = deserialise<test_serialisable>(intermediate);

    assert(second.test_datamember == ser.test_datamember);
}

template<bool auth>
struct net_test : serialisable
{
    net_interpolate<double, auth> tdouble;

    virtual void serialise(nlohmann::json& data, bool encode) override
    {
        DO_SERIALISE(tdouble);
    }
};

void netinterpolate_test()
{
    net_test<true> my_test;

    my_test.tdouble.set_host(1234);


    nlohmann::json wire = serialise(my_test);

    net_test<false> decode_test;

    decode_test = deserialise<net_test<false>>(wire);

    std::cout << "decode " << decode_test.tdouble.get_interpolated(serialisable::time_ms()) << std::endl;

    Sleep(1000);

    nlohmann::json two;

    my_test.tdouble.set_host(2234);

    two = serialise(my_test);


    //decode_test = deserialise<net_test<false>>(two);

    deserialise(two, decode_test);


    std::cout << "decode2 " << decode_test.tdouble.get_interpolated(serialisable::time_ms() - 100) << std::endl;
}

int main(int argc, char* argv[])
{
    netinterpolate_test();

    connection server;
    server.host("127.0.0.1", 11000);

    connection client;
    client.connect("127.0.0.1", 11000);

    connection client2;
    client2.connect("127.0.0.1", 11000);

    //client.write("test");

    //client2.write("test2");

    serialise_test();

    test_serialisable test_network;
    test_network.test_datamember = 23;

    client2.writes_to(test_network, -1);

    client2.write(nlohmann::json("hello").dump());
    client2.write(nlohmann::json("hello2").dump());
    client2.write(nlohmann::json("hello3").dump());

    client.write(nlohmann::json("Test1234").dump());

    while(1)
    {
        if(client.has_read())
        {
            std::cout << "client " << client.read() << std::endl;
            client.pop_read();
        }

        while(server.has_new_client())
        {
            std::cout << "new client\n";

            server.pop_new_client();
        }

        if(client2.has_read())
        {
            std::cout << "client2 " << client2.read() << std::endl;

            /*test_serialisable test;
            test = client2.reads_from<test_serialisable>().data;

            std::cout << "TEST " << test.test_datamember << std::endl;*/

            client2.pop_read();
        }

        if(server.has_read())
        {
            write_data read = server.read_from();

            std::cout << "Server " << read.data << std::endl;
            std::cout << "id " << read.id << std::endl;

            server.write_to(read);

            server.pop_read();
        }
    }

    return EXIT_SUCCESS;
}
