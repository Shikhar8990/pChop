#include "PrefixTree.h"

bool PrefixTree::addToTree(std::vector<unsigned char>& inPath) {
  Node* current = root;
  unsigned int idx=0;
  while(idx<inPath.size()) {
    if(inPath[idx] == '0') {
      if(!current->left) {
        current->left = new Node(nullptr, nullptr);
      }
      current = current->left;
    } else {
      if(!current->right) {
        current->right = new Node(nullptr, nullptr);
      }
      current = current->right;
    }
    idx++;
  }
  return true;
}

bool PrefixTree::getPathToResume(std::vector<unsigned char>& inPath, std::vector<unsigned char>& outPath, std::ostream& log) {
  //1 is right and 0 is left
  //std::string instr(inPath.begin(), inPath.end());
  //log<<"Getting Path: "<<instr<<"\n";
  Node* current = root;
  unsigned int idx=0;
  while(idx<inPath.size()) {
    if(inPath[idx] == '0') {
      //log<<"Idx: "<<idx<<" is 0\n";
      //log.flush();
      if(current->left) {
        outPath.push_back('0');
        current = current->left;
      } else {
        break;
      }
    } else {
      //log<<"Idx: "<<idx<<" is 1\n";
      //log.flush();
      if(current->right) {
        outPath.push_back('1');
        current = current->right;
      } else {
        break;
      }
    }
    ++idx;
  }
  return true;
}
