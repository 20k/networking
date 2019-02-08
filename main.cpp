#include "networking.hpp"
#include "serialisable.hpp"
#include <iostream>
#include <windows.h>

int main(int argc, char* argv[])
{
    connection server;
    server.host("127.0.0.1", 11000);

    connection client;
    client.connect("127.0.0.1", 11000);

    connection client2;
    client2.connect("127.0.0.1", 11000);

    client.write("test");

    client2.write("test2");

    test_serialisable ser;
    ser.test_datamember = 5;

    nlohmann::json intermediate = serialise(ser);

    test_serialisable second = deserialise<test_serialisable>(intermediate);

    std::cout << "test ser " << second.test_datamember << std::endl;

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
            client2.pop_read();
        }

        if(server.has_read())
        {
            write_data read = server.read_from();

            std::cout << read.data << std::endl;
            std::cout << "id " << read.id << std::endl;

            server.write_to(read);

            server.pop_read();
        }
    }

    return EXIT_SUCCESS;
}
