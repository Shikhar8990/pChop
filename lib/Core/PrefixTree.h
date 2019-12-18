#ifndef KLEE_PREFIX_TREE
#define KLEE_PREFIX_TREE
#include <vector>
#include <iostream>
class PrefixTree {
  public:
  class Node {
    public:
    Node* left;
    Node* right;
    Node(Node* l, Node* r) {
      left = l;
      right = r;
    }
  };

  PrefixTree() {
    root = new Node(nullptr, nullptr);
  }

  bool addToTree(std::vector<unsigned char>& inPath);
  bool getPathToResume(std::vector<unsigned char>& inPath, std::vector<unsigned char>& outPath, std::ostream& log);
  private:
  Node* root;
};
#endif
