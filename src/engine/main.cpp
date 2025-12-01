#include "engine/engine.hpp"
#include <iostream>

int main(int argc, char** argv)
{
    std::string host = "0.0.0.0";
    std::string port = "9001";
    size_t top_n = 5;

    // usage: engine_app <port> <topN> [metrics_csv] [log_every]
    if (argc > 1) port = argv[1];
    if (argc > 2) top_n = static_cast<size_t>(std::stoul(argv[2]));

    try
    {
        engine::EngineApp app;
        if (argc > 3)
        {
            std::string metrics_csv = argv[3];
            size_t every = (argc > 4) ? static_cast<size_t>(std::stoul(argv[4])) : 1000;
            app.enable_csv_metrics(metrics_csv, every);
            std::cout << "[engine] CSV metrics -> " << metrics_csv
                        << " (every " << every << " events)\n";
        }
        return app.run(host, port, top_n);
    }
    catch (const std::exception& e)
    {
        std::cerr << "engine error: " << e.what() << "\n";
        return 1;
    }
}
