#ifndef DATATABLE_COLMAJOR_H
#define DATATABLE_COLMAJOR_H

#include "CSRGraph.hpp"
#include "Graph.hpp"
#include "IndexSys.hpp"
#include "Helper.hpp"

using namespace std;

class DataTableColMajor
{
    public:

        typedef int32_t idxType;
        DataTableColMajor(): _subTempsList(nullptr), _dataTable(nullptr), 
        _tableLen(nullptr), _curTable(nullptr), _curMainTable(nullptr), 
        _curAuxTable(nullptr), _indexer(nullptr), _thdNum(1), _blockSizeBasic(1), _blockSize(nullptr),
        _blockPtrDst(nullptr), _blockPtrDstLast(nullptr), _blockPtrA(nullptr), _blockPtrB(nullptr), _useSPMM(0), _bufMatCols(0) {}

        ~DataTableColMajor () {cleanTable();}

        void initDataTable(Graph* subTempsList, IndexSys* indexer, int subsNum, int colorNum, idxType vertsNum, int thdNum, int useSPMM, int bufMatCols);
        void initSubTempTable(int subsId);
        void initSubTempTable(int subsId, int mainId, int auxId);
        void cleanSubTempTable(int subsId, bool isBottom);
        void cleanTable();

        int getMainLen(){ return _curMainLen;}
        int getAuxLen(){ return _curAuxLen; }
        int getTableLen(int subsId) { return _tableLen[subsId]; }
        bool isInited() { return _isInited; }
        bool isSubInited(int subsId) { return _isSubInited[subsId]; }
        bool isVertInitMain(int vertId) {return (_curMainTable[vertId] != nullptr) ? true: false;}
        bool isVertInitAux(int vertId) {return (_curAuxTable[vertId] != nullptr) ? true: false;}

        float* getTableArray(int subsId, int colIdx) {return _dataTable[subsId][colIdx];}
        float* getCurTableArray(int colIdx) {return _curTable[colIdx];}
        float* getMainArray(int colIdx) {return _curMainTable[colIdx];}
        float* getAuxArray(int colIdx) {return _curAuxTable[colIdx];}

        void setTableArray(int subsId, int colIdx, float*& vals);
        void setCurTableArray(int colIdx, float*& vals);
        void setCurTableArrayZero(int colIdx);
        void setMainArray(int colIdx, float*& vals);
        void setAuxArray(int colIdx, float*& vals);
        void countCurBottom(int*& idxCToC, int*& colorVals);
    
        void arrayWiseFMA(float* dst, float* a, float* b);
        void arrayWiseFMAScale(float* dst, float* a, float* b, float scale);
        void arrayWiseFMALast(double* dst, float* a, float* b);
        void arrayWiseFMAAVX(float* dst, float* a, float* b);
        void arrayWiseFMANaive(float* dst, float* a, float* b);
        void arrayWiseFMANaiveAVX(float* dst, float* a, float* b);
        void updateArrayVec(float*& src, float*& dst);

    private:


        Graph* _subTempsList;
        IndexSys* _indexer;
        int _colorNum;
        int _subsNum;
        idxType _vertsNum;
        bool _isInited;
        bool* _isSubInited;
        int _thdNum;
        // block-wise data copy
        int _blockSizeBasic;
        int* _blockSize;

        // the _dataTable is not stored in a column-majored way
        float*** _dataTable;
        int* _tableLen;
        int _curMainLen;
        int _curAuxLen;
        int _curSubId;
        float** _curTable;
        float** _curMainTable;
        float** _curAuxTable;
        float** _blockPtrDst;
        double** _blockPtrDstLast;
        float** _blockPtrA;
        float** _blockPtrB;

        int _useSPMM;
        int _bufMatCols;
        

};


#endif
