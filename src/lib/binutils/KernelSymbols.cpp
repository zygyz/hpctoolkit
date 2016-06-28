//******************************************************************************
// system include files
//******************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <iostream>
#include <vector>
#include <algorithm>



//******************************************************************************
// local include files
//******************************************************************************

#include <include/linux_info.h>
#include "KernelSymbols.hpp"



//******************************************************************************
// types
//******************************************************************************

class KernelSymbol {
public:
  KernelSymbol(uint64_t __addr, char __type, const char *__name); 
  std::string &name();
  uint64_t addr();
  void dump();

private:
  uint64_t _addr;
  char _type;
  std::string _name;
};


struct KernelSymbolsRepr {
  std::vector<KernelSymbol*> kernel_symbols; 
};



//******************************************************************************
// private operations
//******************************************************************************
KernelSymbol::KernelSymbol(
  uint64_t __addr, 
  char __type, 
  const char *__name) 
  : _addr(__addr), _type(__type), _name(__name) 
{
}
  
void  
KernelSymbol::dump() 
{
    std::cout << std::hex << _addr << std::dec 
	      << " " << _type << " " << _name << std::endl;
}


std::string &
KernelSymbol::name()
{
  return _name;
}


uint64_t
KernelSymbol::addr()
{
  return _addr;
}



//******************************************************************************
// interface operations
//******************************************************************************

KernelSymbols::KernelSymbols() 
{
  R = new struct KernelSymbolsRepr;
}


bool
KernelSymbols::parseLinuxKernelSymbols()
{
  FILE *fp = fopen(LINUX_KERNEL_SYMBOL_FILE, "r");

  if (fp) {
    for(;;) {
      char type;
      void *addr;
      char name_buffer[4096];
      int result = fscanf(fp, "%p %c %s", &addr, &type, name_buffer);
      if (result == EOF || result < 3) break;
      switch(type) {
      case 't':
      case 'T':
	R->kernel_symbols.push_back(new KernelSymbol((uint64_t) addr, type, name_buffer));
      default:
	break;
      }
    }
    fclose(fp);
  }

  return R->kernel_symbols.size() > 0;
}


bool
KernelSymbols::find(uint64_t vma, std::string &fnname)
{
  int first = 0;
  int last = R->kernel_symbols.size() - 1;

  if (vma < R->kernel_symbols[first]->addr()) {
    return false;
  }

  if (vma >= R->kernel_symbols[last]->addr()) {
    fnname = R->kernel_symbols[last]->name();
    return true;
  }

  for(;;) { 
    int mid = (first + last + 1) >> 1;
    if (vma >= R->kernel_symbols[mid]->addr()) {
      first = mid;
    } else if (vma < R->kernel_symbols[mid]->addr()) {
      last = mid;
    } 
    if (last - first <= 1) {
      fnname = R->kernel_symbols[first]->name();
      return true;
    }
  }
}



void 
KernelSymbols::dump()
{
  for (auto it = R->kernel_symbols.begin(); it != R->kernel_symbols.end(); ++it) {
    (*it)->dump();
  }
}



//******************************************************************************
// unit test
//******************************************************************************
// #define UNIT_TEST

#ifdef UNIT_TEST

main()
{
  KernelSymbols syms;
  syms.parseLinuxKernelSymbols();
  syms.dump();
  
  uint64_t addr;

  scanf("%p", &addr);
  
  std::string name;
  bool result = syms.find(addr, name);
  std::cout << "Lookup " << std::hex << "0x" << addr << std::dec 
	    << " (" << result << ")" << " --> " << name << std::endl; 
}

#endif
