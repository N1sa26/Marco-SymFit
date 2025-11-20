#include <iostream>

std::string test_symbolic(std::string name) {
  std::string new_name = name + "newstring";
  return new_name;
}

int test_arithmetic(int input) {
  int a = 0xdeadbeef;
  int b = 0xbeefdead;
  int c = a + b;
  int d = input + a;
  int h = d + input;
  if (d > 0xdeadbeef) {
    input += 0xdead;
  } else {
    input -= 0xdead;
  }
  int e = input + b;
  int f = a + b;
  return f;
}

int main(int argc, char *argv[]) {
  int input;
  std::cin >> input;
  int re = test_arithmetic(input);
  /*
  if (input == 10) {
    std::cout << "Bingo" << std::endl;
  } else {
    std::cout << "incorrect" << std::endl;
  }*/
  std::cout << "ret" << re << std::endl;
  return 0;
}
/*
int main(int argc, char *argv[]) {
  std::cout << "What's your name?" << std::endl;
  std::string name;
  std::string name2;
  std::cin >> name;

  name2 = test_symbolic(name);
  std::cout << "test artithemetic " << test_arithmetic() << "!" << std::endl;
  if (name == "root")
    std::cout << "What is your command?" << std::endl;
  else
    std::cout << "Hello, " << name << "!" << std::endl;
  
  std::cout << "new string " << name2 << "!" << std::endl;
  return 0;
}
*/