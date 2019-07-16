#ifndef CSCGRAPH_H
#define CSCGRAPH_H

#include <stdint.h>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <omp.h>
#include "Helper.hpp"

#ifdef __INTEL_COMPILER
// use avx intrinsics
#include "immintrin.h"
#include "zmmintrin.h"
#endif

using namespace std;

template<class idxType, class valType>
class CSCGraph
{
    public:

        CSCGraph(): _isDirected(false), _isOneBased(false), _numEdges(-1), _numVertices(-1), _nnZ(-1), 
        _edgeVal(nullptr), _indexRow(nullptr), _indexCol(nullptr), 
        _degList(nullptr), _numsplits(0), _splitsRowIds(nullptr), _splitsColIds(nullptr), _splitsVals(nullptr) {}

        ~CSCGraph () {

            if (_edgeVal != nullptr)
                free(_edgeVal);

            if (_indexRow != nullptr)
                free(_indexRow);

            if (_indexCol != nullptr)
                free(_indexCol);

            if (_splitsRowIds != nullptr)
                delete[] _splitsRowIds;

            if (_splitsColIds != nullptr)
                delete[] _splitsColIds; 

            if (_splitsVals != nullptr)
                delete[] _splitsVals; 
        }

        valType* getEdgeVals(idxType colId) {return _edgeVal + _indexCol[colId]; }
        idxType* getRowIdx(idxType colId) {return _indexRow + _indexCol[colId]; }
        idxType getColLen(idxType colId) {return _indexCol[colId+1] - _indexCol[colId]; }

        idxType getNumVertices() {return  _numVertices;} 
        idxType getNNZ() {return _indexCol[_numVertices]; }
        idxType* getIndexRow() {return _indexRow;}
        idxType* getIndexCol() {return _indexCol;}
        valType* getNNZVal() {return _edgeVal;} 

        idxType* getDegList() {return _degList;}

        void createFromEdgeListFile(idxType numVerts, idxType numEdges, 
                idxType* srcList, idxType* dstList, bool isBenchmark = false);       

        void splitCSC(idxType numsplits);
        void spmvNaiveSplit(valType* x, valType* y, idxType numThds);
        double spmmSplitExp(valType* x, valType* y, idxType xColNum, idxType numThds);
        void spmmSplit(valType* x, valType* y, idxType xColNum, idxType numThds);

        void serialize(ofstream& outputFile);
        void deserialize(int inputFile);

    private:
        /* data */

        bool _isDirected;
        bool _isOneBased;
        idxType _numEdges;
        idxType _numVertices;
        idxType _nnZ;
        valType* _edgeVal;
        idxType* _indexRow;
        idxType* _indexCol;
        idxType* _degList;
        idxType _numsplits;

        std::vector<idxType>* _splitsRowIds;
        std::vector<idxType>* _splitsColIds;
        std::vector<valType>* _splitsVals;

};

template<class idxType, class valType>
void CSCGraph<idxType, valType>::createFromEdgeListFile(idxType numVerts, idxType numEdges, idxType* srcList, idxType* dstList, bool isBenchmark) 
{
    _numEdges = numEdges;
    _numVertices = numVerts;

    _degList = (idxType*) malloc(_numVertices*sizeof(idxType)); 

#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < _numVertices; ++i) {
        _degList[i] = 0;
    }

    // build the degree list
#pragma omp parallel for
    for(idxType i = 0; i< _numEdges; i++)
    {
        idxType srcId = srcList[i];
        idxType dstId = dstList[i];

#pragma omp atomic
        _degList[dstId]++;

        // non-directed graph
        if (!_isDirected)
        {
#pragma omp atomic
        _degList[srcId]++;
        }
    }

    // calculate the col index of CSC (offset)
    // size numVerts + 1
    _indexCol = (idxType*)malloc((_numVertices+1)*sizeof(idxType));
    _indexCol[0] = 0;
    for(idxType i=1; i<= _numVertices;i++)
        _indexCol[i] = _indexCol[i-1] + _degList[i-1]; 

    // create the row index and val 
    _nnZ = _indexCol[_numVertices];

    _indexRow = (idxType*)malloc(_indexCol[_numVertices]*sizeof(idxType));
    std::vector<idxType> indexRowVec(_indexCol[_numVertices]);

    _edgeVal = (valType*)malloc(_indexCol[_numVertices]*sizeof(valType));

#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < _indexCol[_numVertices]; ++i) {
        _edgeVal[i] = 0;
    }

    for(idxType i = 0; i< _numEdges; i++)
    {

        idxType srcId = srcList[i];
        idxType dstId = dstList[i];

        indexRowVec[_indexCol[dstId]] = srcId;
        _edgeVal[(_indexCol[dstId])++] = 1.0;

        // non-directed graph
        if (!_isDirected)
        {
            indexRowVec[_indexCol[srcId]] = dstId;
            _edgeVal[(_indexCol[srcId])++] = 1.0;
        }
    }

    // recover the indexRow 
#pragma omp parallel for
    for(idxType i=0; i<_numVertices;i++)
        _indexCol[i] -= _degList[i];

    // sort the row id for each col
    // no need to sort the val in adjacency matrix
#pragma omp parallel for
    for (idxType i = 0; i < _numVertices; ++i) {

        // is this thread safe ?
        std::sort(indexRowVec.begin()+_indexCol[i], indexRowVec.begin()+_indexCol[i+1]);
    }

    std::copy(indexRowVec.begin(), indexRowVec.end(), _indexRow);
}

template<class idxType, class valType>
void CSCGraph<idxType, valType>::splitCSC(idxType numsplits)
{
    double splitt0 = utility::timer();
    std::cout << "splitCSCStart: " << splitt0 << std::endl;
    _numsplits = numsplits;
    if (_splitsRowIds != nullptr)
        delete[] _splitsRowIds;

    if (_splitsColIds != nullptr)
        delete[] _splitsColIds;

    if (_splitsVals != nullptr)
        delete[] _splitsVals;

    _splitsRowIds = new std::vector<idxType>[_numsplits];
    _splitsColIds = new std::vector<idxType>[_numsplits];
    _splitsVals = new std::vector<valType>[_numsplits];

    idxType perpiece = _numVertices / _numsplits;

    std::cout << "In splitCSCStart - Before loop: " << utility::timer() << std::endl; 
    for (idxType i=0; i < _numVertices; ++i)
    {
        for (idxType j = _indexCol[i]; j < _indexCol[i+1]; ++j)
        {
            // already sorted
            idxType rowid = _indexRow[j];
            idxType owner = std::min(rowid / perpiece, (_numsplits-1));
            _splitsColIds[owner].push_back(i);
            _splitsRowIds[owner].push_back(rowid);
            _splitsVals[owner].push_back(_edgeVal[j]);
        }
    }
    double splitt1 = utility::timer();
    std::cout << "splitCSCFinish: " << splitt1 << std::endl;
    std::cout << "splitCSC Time cost (s): " << splitt1 - splitt0 << std::endl;
}

template<class idxType, class valType>
void CSCGraph<idxType, valType>::spmvNaiveSplit(valType* x, valType* y, idxType numThds)
{
    // split CSC spmv
#pragma omp parallel for num_threads(numThds) 
    for (idxType s = 0; s < _numsplits; ++s) {

        std::vector<idxType>* localRowIds = &(_splitsRowIds[s]);
        std::vector<idxType>* localColIds = &(_splitsColIds[s]);
        std::vector<valType>* localVals = &(_splitsVals[s]);
        idxType localSize = localRowIds->size();

// here the usage of simd will cause write conflict on rowid
        for (idxType j = 0; j < localSize; ++j) {
            idxType colid = (*localColIds)[j];
            idxType rowid = (*localRowIds)[j];
            valType val = (*localVals)[j];
            y[rowid] += (val*x[colid]);
        }
    }
}

// sparse matrix dense matrix (multiple dense vectors) 
template<class idxType, class valType>
void CSCGraph<idxType, valType>::spmmSplit(valType* x, valType* y, idxType xColNum, idxType numThds)
{
    // doing the computation
#pragma omp parallel for num_threads(numThds) 
    for (idxType s = 0; s < _numsplits; ++s) {

        std::vector<idxType>* localRowIds = &(_splitsRowIds[s]);
        std::vector<idxType>* localColIds = &(_splitsColIds[s]);
        // std::vector<valType>* localVals = &(_splitsVals[s]);
        idxType localSize = localRowIds->size();

        for (idxType j = 0; j < localSize; ++j) 
        {
            idxType colid = (*localColIds)[j];
            idxType rowid = (*localRowIds)[j];
            // valType val = (*localVals)[j];

            valType* readBufPtr = x + colid*xColNum;
            valType* writeBufPtr = y + rowid*xColNum;

#ifdef __INTEL_COMPILER
        __assume_aligned(readBufPtr, 64);
        __assume_aligned(writeBufPtr, 64);
#else
        __builtin_assume_aligned(readBufPtr, 64);
        __builtin_assume_aligned(writeBufPtr, 64);
#endif

#ifdef __AVX512F__ 
            // unrolled by 16 float
            int n16 = xColNum & ~(16-1); 
            __m512 tmpzero = _mm512_set1_ps (0);
            __mmask16 mask = (1 << (xColNum - n16)) - 1;

            __m512 vecX;
            __m512 vecY;
            __m512 vecBuf;

            for (int k = 0; k < n16; k+=16)
            {
                vecX = _mm512_load_ps (&(readBufPtr[k]));
                vecY = _mm512_load_ps (&(writeBufPtr[k]));
                vecBuf = _mm512_add_ps(vecX, vecY);
                _mm512_store_ps(&(writeBufPtr[k]), vecBuf);
            }

            if (n16 < xColNum)
            {
                vecX = _mm512_mask_load_ps(tmpzero, mask, &(readBufPtr[n16]));
                vecY = _mm512_mask_load_ps(tmpzero, mask, &(writeBufPtr[n16]));
                vecBuf = _mm512_add_ps(vecX, vecY);
                _mm512_mask_store_ps(&(writeBufPtr[n16]), mask, vecBuf);
            }

#else
            // compiler auto vectorization
// #pragma omp simd
//#pragma omp simd aligned(writeBufPtr, readBufPtr: 64)
            for (int k = 0; k < xColNum; ++k) {
                writeBufPtr[k] += (readBufPtr[k]); 
            }
#endif


        }
    }

}

// sparse matrix dense matrix (multiple dense vectors) 
// used in benchmarking
template<class idxType, class valType>
double CSCGraph<idxType, valType>::spmmSplitExp(valType* x, valType* y, idxType xColNum, idxType numThds)
{
    double startTime = 0.0;
    double conversionTime = 0.0;
    double computeTime = 0.0;

    // data format conversion
    // creating the first buffer
    valType* readBuf = (valType*) _mm_malloc(_numVertices*xColNum*sizeof(valType), 64);
    valType* writeBuf = (valType*) _mm_malloc(_numVertices*xColNum*sizeof(valType), 64);

    startTime = utility::timer();
    // convert x from column majored to row majord
#pragma omp parallel for
    for (int i = 0; i < _numVertices; ++i) {
        for (int j = 0; j < xColNum; ++j) {
            readBuf[i*xColNum+j] = x[j*_numVertices+i];
        }
    }

    conversionTime += (utility::timer() - startTime);
    startTime = utility::timer();

    // doing the computation
#pragma omp parallel for num_threads(numThds) 
    for (idxType s = 0; s < _numsplits; ++s) 
    {

        std::vector<idxType>* localRowIds = &(_splitsRowIds[s]);
        std::vector<idxType>* localColIds = &(_splitsColIds[s]);
        // std::vector<valType>* localVals = &(_splitsVals[s]);
        idxType localSize = localRowIds->size();


        for (idxType j = 0; j < localSize; ++j) 
        {
            idxType colid = (*localColIds)[j];
            idxType rowid = (*localRowIds)[j];
            // valType val = (*localVals)[j];

            valType* readBufPtr = readBuf + colid*xColNum;
            valType* writeBufPtr = writeBuf + rowid*xColNum;

#ifdef __INTEL_COMPILER
            __assume_aligned(readBufPtr, 64);
            __assume_aligned(writeBufPtr, 64);
#else
            __builtin_assume_aligned(readBufPtr, 64);
            __builtin_assume_aligned(writeBufPtr, 64);
#endif

// #ifdef __AVX512F__ 
//             // unrolled by 16 float
//             int n16 = xColNum & ~(16-1); 
//             __m512 tmpzero = _mm512_set1_ps (0);
//             __mmask16 mask = (1 << (xColNum - n16)) - 1;
//
//             __m512 vecX;
//             __m512 vecY;
//             __m512 vecBuf;
//
//             for (int k = 0; k < n16; k+=16)
//             {
//                 vecX = _mm512_load_ps (&(readBufPtr[k]));
//                 vecY = _mm512_load_ps (&(writeBufPtr[k]));
//                 vecBuf = _mm512_add_ps(vecX, vecY);
//                 _mm512_store_ps(&(writeBufPtr[k]), vecBuf);
//             }
//
//             if (n16 < xColNum)
//             {
//                 vecX = _mm512_mask_load_ps(tmpzero, mask, &(readBufPtr[n16]));
//                 vecY = _mm512_mask_load_ps(tmpzero, mask, &(writeBufPtr[n16]));
//                 vecBuf = _mm512_add_ps(vecX, vecY);
//                 _mm512_mask_store_ps(&(writeBufPtr[n16]), mask, vecBuf);
//             }
//
// #else
            // compiler auto vectorization
#pragma omp simd aligned(writeBufPtr, readBufPtr: 64)
            for (int k = 0; k < xColNum; ++k) {
                writeBufPtr[k] += readBufPtr[k]; 
            }
// #endif

        }
    }

    computeTime += (utility::timer() - startTime);
    startTime = utility::timer();

    // convert writeBuf back to y 
#pragma omp parallel for
    for (int i = 0; i < _numVertices; ++i) {
        for (int j = 0; j < xColNum; ++j) {
            y[j*_numVertices+i] = writeBuf[i*xColNum+j];
        }
    }   

    conversionTime += (utility::timer() - startTime);

    _mm_free(readBuf);
    _mm_free(writeBuf);

    printf("Compute Time: %f sec, Conversion Time: %f sec\n", 
            computeTime, conversionTime);
    std::fflush(stdout);           

    return computeTime;
}


template<class idxType, class valType>
void CSCGraph<idxType, valType>::serialize(ofstream& outputFile)
{
    outputFile.write((char*)&_numEdges, sizeof(idxType));
    outputFile.write((char*)&_numVertices, sizeof(idxType));
    outputFile.write((char*)_degList, _numVertices*sizeof(idxType));
    outputFile.write((char*)_indexCol, (_numVertices+1)*sizeof(idxType));
    outputFile.write((char*)_indexRow, (_indexCol[_numVertices])*sizeof(idxType));
    outputFile.write((char*)_edgeVal, (_indexCol[_numVertices])*sizeof(valType));
}
        
template<class idxType, class valType>
void CSCGraph<idxType, valType>::deserialize(int inputFile)
{

    read(inputFile, (char*)&_numEdges, sizeof(idxType));
    read(inputFile, (char*)&_numVertices, sizeof(idxType));

    _degList = (idxType*) malloc (_numVertices*sizeof(idxType)); 
    read(inputFile, (char*)_degList, _numVertices*sizeof(idxType));

    _indexCol = (idxType*) malloc ((_numVertices+1)*sizeof(idxType)); 
    read(inputFile, (char*)_indexCol, (_numVertices+1)*sizeof(idxType));

    _indexRow = (idxType*) malloc (_indexCol[_numVertices]*sizeof(idxType)); 
    read(inputFile, (char*)_indexRow, (_indexCol[_numVertices])*sizeof(idxType));

    _edgeVal = (valType*) malloc ((_indexCol[_numVertices])*sizeof(valType)); 
    read(inputFile, (char*)_edgeVal, (_indexCol[_numVertices])*sizeof(valType));

    _nnZ = _indexCol[_numVertices];

    printf("CSC Format Total vertices is : %d\n", _numVertices);
    printf("CSC Format Total Edges is : %d\n", _numEdges);
    std::fflush(stdout); 

}

#endif
