#ifndef Cuda_hpp

#include <string>
#include <string>

class CudaSymbols {
public:
  CudaSymbols();
  bool parseCudaSymbols();
  bool find(uint64_t vma, std::string &fnname, uint32_t &line, std::string &filename);
  void dump();
private:
  struct CudaSymbolsRepr *R;
};

#endif
