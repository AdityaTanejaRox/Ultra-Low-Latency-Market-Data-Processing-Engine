#pragma once
#include <string>

namespace streamer
{

    // Step1:- For now, read a text file containing our simple line protocol and stream to engine.
    // Step2:- I'll replace the source with a DBN reader.
    class Streamer
    {
    public:
        int run(const std::string& host, const std::string& port, const std::string& input_file, size_t lines_per_sec);
    };

} // namespace streamer
