#include "streamer/streamer.hpp"
#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: streamer_app <engine_port> <input_txt> [lines_per_sec]\n";
        return 1;
    }
    std::string host = "127.0.0.1";
    std::string port = argv[1];
    std::string input = argv[2];
    size_t lps = (argc > 3) ? static_cast<size_t>(std::stoul(argv[3])) : 100000;

    try
    {
        streamer::Streamer s;
        return s.run(host, port, input, lps);
    }
    catch (const std::exception& e)
    {
        std::cerr << "streamer error: " << e.what() << "\n";
        return 1;
    }
}
