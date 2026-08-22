#include "aqua/logger/logger.h"

int main()
{
    aqua::init_logger();
    aqua::set_log_level(aqua::default_log_level());

    // TODO: 启动 client
    return 0;
}
