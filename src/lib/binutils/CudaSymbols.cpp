//******************************************************************************
// system include files
//******************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <vector>
#include <algorithm>

//******************************************************************************
// local include files
//******************************************************************************

#include "CudaSymbols.h"
#include "CudaSymbols.hpp"

//******************************************************************************
// types
//******************************************************************************

class CudaSymbol {
public:
  CudaSymbol(uint32_t __functionId, uint32_t __pcOffset, 
	     uint32_t __moduleId, uint32_t __functionIndex, 
	     uint32_t __sourceLocatorId, const char *__name, 
	     uint32_t __line, const char *filename, const char *stall);

  uint32_t functionId();
  uint32_t pcOffset();
  uint32_t moduleId();
  uint32_t functionIndex();
  uint32_t sourceLocatorId();
  std::string &name();
  uint32_t &line();
  std::string &filename();
  std::string &stall();
  void dump();  

private:
  uint32_t _functionId;
  uint32_t _pcOffset;
  uint32_t _moduleId;
  uint32_t _functionIndex;
  uint32_t _sourceLocatorId;
  std::string _name;
  uint32_t _line;
  std::string _filename;
  std::string _stall;
};


struct CudaSymbolsRepr {
  std::vector<CudaSymbol*> cuda_symbols; 
};

//******************************************************************************
// private operations
//******************************************************************************

CudaSymbol::CudaSymbol(
  uint32_t __functionId, 
  uint32_t __pcOffset,
  uint32_t __moduleId,
  uint32_t __functionIndex,
  uint32_t __sourceLocatorId,
  const char *__name,
  uint32_t __line,
  const char *__filename,
  const char *__stall) : 
  _functionId(__functionId), _pcOffset(__pcOffset), 
  _moduleId(__moduleId), _functionIndex(__functionIndex), 
  _sourceLocatorId(__sourceLocatorId), _name(__name), 
  _line(__line), _filename(__filename), _stall(__stall)
{
}
  
void  
CudaSymbol::dump() 
{
  std::cout << _functionId << " " << _pcOffset << " " << _moduleId << " " 
	    << _functionIndex << " " << _sourceLocatorId << std::endl;
}


uint32_t
CudaSymbol::functionId()
{
  return _functionId;
}

uint32_t
CudaSymbol::pcOffset()
{
  return _pcOffset;
}

uint32_t
CudaSymbol::moduleId()
{
  return _moduleId;
}

uint32_t
CudaSymbol::functionIndex()
{
  return _functionIndex;
}

uint32_t
CudaSymbol::sourceLocatorId()
{
  return _sourceLocatorId;
}

std::string &
CudaSymbol::name()
{
  return _name;
}

uint32_t &
CudaSymbol::line()
{
  return (uint32_t&)_line;
}

std::string &
CudaSymbol::filename()
{
  return _filename;
}

std::string &
CudaSymbol::stall()
{
  return _stall;
}

static bool
compare(CudaSymbol *r1, CudaSymbol *r2)
{
  uint64_t r1_merge;
  uint64_t r2_merge;

  uint32_t r1_fid;
  uint32_t r1_pc;
  uint32_t r2_fid;
  uint32_t r2_pc;

  r1_fid = r1->functionId();
  r1_pc = r1->pcOffset();
  r2_fid = r2->functionId();
  r2_pc = r2->pcOffset();

#if 0
  uint32_t md;
  md = r1->moduleId();
  printf("md: %u\n", md);
#endif

  r1_merge = ( ((uint64_t)r1_fid) << 32 | r1_pc);	
  r2_merge = ( ((uint64_t)r2_fid) << 32 | r2_pc);

  return r1_merge < r2_merge;	
}

//******************************************************************************
// interface operations
//******************************************************************************

CudaSymbols::CudaSymbols() 
{
  R = new struct CudaSymbolsRepr;
}


bool
CudaSymbols::parseCudaSymbols()
{
  FILE *fp = fopen(CUDA_BINARY_SYMBOL_FILE, "r");

  if (fp) {
	
    size_t len = 4096;
    char *line = (char *) malloc(len); 
    
    for(;;) {
      if (getline(&line, &len, fp) == EOF) break;
      uint32_t fid;
      uint32_t pc;
      uint32_t mid;
      uint32_t findex;
      uint32_t slid;
      char name[4096];
      uint32_t ln;
      char filename[4096];
      char stall[4096];
      int result = sscanf(line, "%u %u %u %u %u %s %u %s %s\n", 
			  &fid, &pc, &mid, &findex, &slid, name, &ln, 
			  filename, stall);
      R->cuda_symbols.push_back(new CudaSymbol(fid, pc, mid, findex, 
					       slid, name, ln, filename, 
					       stall));
    }
    fclose(fp);
  }
  int size = R->cuda_symbols.size();

  if (size > 1) {
    std::sort (R->cuda_symbols.begin(), R->cuda_symbols.end(), compare);
  }

  return size > 0;
}

#if 1
bool
CudaSymbols::find
(
 uint64_t vma, 
 std::string &fnname, 
 uint32_t &line, 
 std::string &filename
)
{
  uint32_t test1, test2;
  test1 = (uint32_t)(vma>>32);
  test2 = (uint32_t)vma;
  //printf("In find, unpack vma, top 32 is %u low 32 is %u\n", test1, test2);

  int first = 0;
  int last = R->cuda_symbols.size() - 1;

  uint32_t first_fid;
  uint32_t first_pc;
  uint64_t first_merge;
  first_fid = R->cuda_symbols[first]->functionId();
  first_pc = R->cuda_symbols[first]->pcOffset();
  first_merge = ( ((uint64_t)first_fid) << 32 | first_pc);	

  uint32_t last_fid;
  uint32_t last_pc;
  uint64_t last_merge;
  last_fid = R->cuda_symbols[last]->functionId();
  last_pc = R->cuda_symbols[last]->pcOffset();
  last_merge = ( ((uint64_t)last_fid) << 32 | last_pc);	

  if (vma < first_merge) {
#if 1
    printf("find false\n");
    return false;
#endif
#if 0
    for (int i = first; i <= last; i++){
      std::string &fnn = R->cuda_symbols[i]->name();
      if (fnn[2]=='o'){
	fnname = R->cuda_symbols[i]->name();
	line = R->cuda_symbols[i]->line();
	filename = R->cuda_symbols[i]->filename();
	//printf("so what?\n");
	return true;
      }
    }
#endif
    /* 10/11/2016
       fnname = R->cuda_symbols[0]->name();
       line = R->cuda_symbols[0]->line();
       filename = R->cuda_symbols[0]->filename();
    */
  }

  if (vma >= last_merge) {
    fnname = R->cuda_symbols[last]->name();
    line = R->cuda_symbols[last]->line();
    filename = R->cuda_symbols[last]->filename();
    //printf("Interpret this record based on current vma: moduleid %u functionindex %u sourcelocatorid %u\n functionname %s filename %s linenumber %u\n", R->cuda_symbols[last]->moduleId(), R->cuda_symbols[last]->functionIndex(), R->cuda_symbols[last]->sourceLocatorId(), fnname.c_str(), filename.c_str(), line);
    return true;
  }

  for(;;) { 
    int mid = (first + last + 1) >> 1;
    uint32_t mid_fid;
    uint32_t mid_pc;
    uint64_t mid_merge;
    mid_fid = R->cuda_symbols[mid]->functionId();
    mid_pc = R->cuda_symbols[mid]->pcOffset();
    mid_merge = ( ((uint64_t)mid_fid) << 32 | mid_pc);
	
    if (vma >= mid_merge) {
      first = mid;
    } else if (vma < mid_merge) {
      last = mid;
    }
    if (last - first <= 1) {
      fnname = R->cuda_symbols[first]->name();
      line = R->cuda_symbols[first]->line();
      filename = R->cuda_symbols[first]->filename();
      //printf("Interpret this record based on current vma: moduleid %u functionindex %u sourcelocatorid %u functionname %s filename %s linenumber %u\n", R->cuda_symbols[first]->moduleId(), R->cuda_symbols[first]->functionIndex(), R->cuda_symbols[first]->sourceLocatorId(), fnname.c_str(), filename.c_str(), line);
      return true;
    }
  }
}
#endif


void 
CudaSymbols::dump()
{
  for (auto it = R->cuda_symbols.begin(); it != R->cuda_symbols.end(); ++it) {
    (*it)->dump();
  }
}

