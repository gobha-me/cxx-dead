#pragma once

namespace framework {

class Application {
  public:
    virtual ~Application() = default;
    void run();
    virtual void on_start() = 0;
};

void unused_framework();

} // namespace framework
