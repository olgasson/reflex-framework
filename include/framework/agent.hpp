#pragma once
namespace reflex {

class Agent {
public:
  virtual ~Agent() = default;

  virtual void on_start() = 0;

  virtual void on_close() = 0;

  virtual int do_work() = 0;
};

}
