#include <iostream>
#include <string>

#include "ubi_cli.h"

int main(int argc, char** argv) {
  UbiInitializeCliProcess();
  std::string error;
  const int exit_code = UbiRunEnvCli(argc, argv, &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
  }
  return exit_code;
}
