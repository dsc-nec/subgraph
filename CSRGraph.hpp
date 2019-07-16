#ifndef CSRGRAPH_H
#define CSRGRAPH_H

#include <stdint.h>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <iostream>

#ifndef NEC
#include "SpDM3/include/spmat.h"
#include "mkl.h"
// for RCM reordering
#include "SpMP/CSR.hpp"
#endif

using namespace std;

// store the edgelist graph data into a CSR format (non-symmetric)
// use float type for entry and avoid the type conversion with count data
class CSRGraph
{
    public:

        typedef int32_t idxType;
        // typedef int idxType;
        typedef float valType;
#ifndef NEC
        CSRGraph(): _isDirected(false), _isOneBased(false), _numEdges(-1), _numVertices(-1), _nnZ(-1),  
            _edgeVal(nullptr), _indexRow(nullptr), _indexCol(nullptr), 
            _degList(nullptr), _useMKL(false), _useRcm(false), _rcmMat(nullptr), _rcmMatR(nullptr) {}
#else
        CSRGraph(): _isDirected(false), _isOneBased(false), _numEdges(-1), _numVertices(-1), _nnZ(-1),  
            _edgeVal(nullptr), _indexRow(nullptr), _indexCol(nullptr), 
            _degList(nullptr), _useMKL(false), _useRcm(false) {}
#endif

        ~CSRGraph(){
            if (_edgeVal != nullptr)
                free(_edgeVal);

            if (_indexRow != nullptr)
                free(_indexRow);

            if (_indexCol != nullptr)
                free(_indexCol);
#ifndef NEC
            if (_rcmMatR != nullptr)
                delete _rcmMatR;
#endif
        }

        valType* getEdgeVals(idxType rowId)
        {
#ifndef NEC
            return (_rcmMatR != nullptr) ? (_rcmMatR->svalues + _rcmMatR->rowptr[rowId]) : (_edgeVal + _indexRow[rowId]); 
#else
            return (_edgeVal + _indexRow[rowId]); 
#endif
        }

        idxType* getColIdx(idxType rowId)
        {
#ifndef NEC
            return (_rcmMatR != nullptr ) ? (_rcmMatR->colidx + _rcmMatR->rowptr[rowId]) : (_indexCol + _indexRow[rowId]);
#else
            return (_indexCol + _indexRow[rowId]);
#endif
        }

        idxType getRowLen(idxType rowId)
        {
#ifndef NEC
            return (_rcmMatR != nullptr ) ? (_rcmMatR->rowptr[rowId + 1] - _rcmMatR->rowptr[rowId]) : (_indexRow[rowId+1] - _indexRow[rowId]);
#else
            return (_indexRow[rowId+1] - _indexRow[rowId]);
#endif
        }

        void SpMVNaive(valType* x, valType* y);
        void SpMVNaiveFull(valType* x, valType* y);
        void SpMVNaiveScale(valType* x, valType* y, float scale);
        void SpMVNaive(valType* x, valType* y, int thdNum);
        void SpMVNaiveFull(valType* x, valType* y, int thdNum);
        void SpMVMKL(valType* x, valType* y, int thdNum);
        void SpMVMKLHint(int callNum);
#ifndef NEC
        idxType getNumVertices() {return (_rcmMatR != nullptr) ? _rcmMatR->m : _numVertices;} 
#else
        idxType getNumVertices() {return _numVertices;} 
#endif

#ifndef NEC
        idxType getNNZ() {return (_rcmMatR != nullptr) ? _rcmMatR->rowptr[_rcmMatR->m] : _indexRow[_numVertices]; }
        idxType* getIndexRow() {return (_rcmMatR != nullptr) ? _rcmMatR->rowptr : _indexRow;}
        idxType* getIndexCol() {return (_rcmMatR != nullptr) ? _rcmMatR->colidx : _indexCol;}
        valType* getNNZVal() {return (_rcmMatR != nullptr) ? _rcmMatR->svalues : _edgeVal;} 
#else
        idxType getNNZ() {return _indexRow[_numVertices]; }
        idxType* getIndexRow() {return _indexRow;}
        idxType* getIndexCol() {return _indexCol;}
        valType* getNNZVal() {return _edgeVal;} 
#endif
         
        void createFromEdgeListFile(idxType numVerts, idxType numEdges, 
                idxType* srcList, idxType* dstList, bool useMKL = false, bool useRcm = false, bool isBenchmark = false);       

        // make the csr format from 0 based index to 1 based index
        // used by csrmm of MKL
        void makeOneIndex();

        idxType* getDegList() {return _degList;}

        void serialize(ofstream& outputFile);
        void deserialize(int inputFile, bool useMKL = false, bool useRcm = false);

#ifndef NEC
        void fillSpMat(spdm3::SpMat<int, float> &smat);
#endif
        void rcmReordering();
        void createMKLMat();
        void toASCII(string fileName);

        bool useMKL() { return _useMKL; }

    private:

        bool _isDirected;
        bool _isOneBased;
        idxType _numEdges;
        idxType _numVertices;
        idxType _nnZ;
        valType* _edgeVal;
        idxType* _indexRow;
        idxType* _indexCol;
        idxType* _degList;
#ifndef NEC
        sparse_matrix_t _mklA;
        matrix_descr _descA;
#endif
        bool _useMKL;

#ifndef NEC
        SpMP::CSR* _rcmMat;
        SpMP::CSR* _rcmMatR;
#endif
        bool _useRcm;
};

#endif
