// the implementation of the color-coding algorithms
// Author: Langshi Chen

#include <cassert>
#include <math.h>
#include<cstdlib>
#include<ctime>
#include <cstring>
#include <omp.h>
#include <vector>
#include <map>
#ifndef NEC
#include "mkl.h"
#endif

#include "CountMat.hpp"
#include "Helper.hpp"

using namespace std;

void CountMat::initialization(CSRGraph* graph, CSCGraph<int32_t, float>* graphCSC, int thd_num, int itr_num, int isPruned, int useSPMM, int vtuneStart, bool calculate_automorphisms)
{
    // either use _graph or _graphCSC
    _graph = graph;
    _graphCSC = graphCSC;

    if (_graph != nullptr)
        _vert_num = _graph->getNumVertices();
    else
        _vert_num = _graphCSC->getNumVertices();

    _thd_num = thd_num;
    _itr_num = itr_num;
    _isPruned = isPruned;
    _useSPMM = useSPMM;
    _isScaled = 0;
    _vtuneStart = vtuneStart;
    _calculate_automorphisms = calculate_automorphisms;

    // mkl spmm use one-based csr
    if (_graph != nullptr && _graph->useMKL() && _useSPMM == 1)
        _graph->makeOneIndex();

    _colors_local = (int*)malloc(_vert_num*sizeof(int));

#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < _vert_num; ++i) {
        _colors_local[i] = 0;
    }

#ifdef __INTEL_COMPILER
    _bufVec = (float*) _mm_malloc(_vert_num*sizeof(float), 64); 
#else
    _bufVec = (float*) aligned_alloc(64, _vert_num*sizeof(float)); 
#endif
#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < _vert_num; ++i) {
        _bufVec[i] = 0;
    }

    // doing 16 SIMD float operations 
    _bufMatCols = 16;

#ifdef __INTEL_COMPILER
    _bufMatY = (float*) _mm_malloc(_vert_num*_bufMatCols*sizeof(float), 64); 
    _bufMatX = (float*) _mm_malloc(_vert_num*_bufMatCols*sizeof(float), 64); 
#else
    _bufMatY = (float*) aligned_alloc(64, _vert_num*_bufMatCols*sizeof(float)); 
    _bufMatX = (float*) aligned_alloc(64, _vert_num*_bufMatCols*sizeof(float)); 
#endif

#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int i = 0; i < _vert_num*_bufMatCols; ++i) {
        _bufMatX[i] = 0;
        _bufMatY[i] = 0;
    }
}

double CountMat::compute(Graph& templates, bool isEstimate)
{/*{{{*/

    _templates = &templates; 
    _color_num = _templates->get_vert_num();

    div_tp.DivideTp(*(_templates));
    div_tp.sort_tps();

    _subtmp_array = div_tp.get_subtps();
    _total_sub_num = div_tp.get_subtps_num();

#ifdef VERBOSE
    printSubTemps(); 
#endif
   
    // create the index tables
    indexer.initialization(_color_num, _total_sub_num, &_subtmp_array, &div_tp);

    // check the effective aux indices
    // for (int s = 0; s < _total_sub_num; ++s) {
    //     printf("Effectiv sub %d\n", s);
    //     std::fflush(stdout);
    //     std::vector<int>* effectVector = indexer.getEffectiveAuxIndices();
    //     for (int i = 0; i < effectVector[s].size(); ++i) {
    //         printf("index: %d\n", effectVector[s][i]); 
    //         std::fflush(stdout);
    //     }
    // }
    // for (int s = 0; s < _total_sub_num; ++s) {
    //
    //     if (_subtmp_array[s].get_vert_num() > 1)
    //     {
    //         int idxAux = div_tp.get_aux_node_idx(s);
    //         int auxSize = indexer.getSubsSize()[idxAux];
    //         int auxNodesLen = indexer.getCombTable()[_color_num][auxSize];
    //         printf("Subs: %d, Aux count len: %d, effect len: %d\n", s, auxNodesLen, (indexer.getEffectiveAuxIndices())[s].size());
    //         std::fflush(stdout);
    //     }
    // }

#ifdef VERBOSE
    printf("Start initializaing datatable\n");
    std::fflush(stdout); 
#endif

    _dTable.initDataTable(_subtmp_array, &indexer, _total_sub_num, _color_num, _vert_num, _thd_num, _useSPMM, _bufMatCols);

#ifdef VERBOSE
    printf("Finish initializaing datatable\n");
    std::fflush(stdout); 
#endif

#ifdef VERBOSE
    // peak memory usage on a single node
    estimatePeakMemUsage();

    // for flops of pruned color-coding
    double totalFlops = estimateFlopsPGBSC();
    double totalMemBand = estimateMemCommPGBSC();

    printf("PGBSC Arith Intensity: %f\n", (totalFlops/totalMemBand));
    std::fflush(stdout);

    // for original color-coding algorithm
    totalFlops = estimateFlopsFascia();
    totalMemBand = estimateMemCommFascia();

    printf("Fascia Arith Intensity: %f\n", (totalFlops/totalMemBand));
    std::fflush(stdout);

    // for original color-coding algorithm
    totalFlops = estimateFlopsPrunedFascia();
    totalMemBand = estimateMemCommPrunedFascia();

    printf("Pruned Fascia Arith Intensity: %f\n", (totalFlops/totalMemBand));
    std::fflush(stdout);

    // for sparse input data distribution
    degreeDistribution();

    estimateTemplate();

#endif 

    // exit without counting
    if (isEstimate)
        return 0.0;

#ifdef VERBOSE
    printf("Start counting\n");
    std::fflush(stdout); 
#endif

    // allocating the bufVecLeaf buffer
    _bufVecLeaf = (float**) malloc (_color_num*sizeof(float*));
    if (_useSPMM == 0)
    {
        for (int i = 0; i < _color_num; ++i) {
#ifdef __INTEL_COMPILER
            _bufVecLeaf[i] =  (float*) _mm_malloc(_vert_num*sizeof(float), 64); 
#else
            _bufVecLeaf[i] =  (float*) aligned_alloc(64, _vert_num*sizeof(float)); 
#endif 
        }

    }
    else
    {
#ifdef __INTEL_COMPILER
        _bufVecLeaf[0] =  (float*) _mm_malloc((int64_t)(_vert_num)*_color_num*sizeof(float), 64); 
#else
        _bufVecLeaf[0] =  (float*) aligned_alloc(64, (int64_t)(_vert_num)*_color_num*sizeof(float)); 
#endif 

        for (int i = 1; i < _color_num; ++i) {
           _bufVecLeaf[i] = _bufVecLeaf[0] + i*_vert_num; 
        }
    }

    // start counting
    double timeStart = utility::timer();

    double iterCount = 0.0;
    for (int i = 0; i < _itr_num; ++i) {
        iterCount += colorCounting();
    }

#ifdef VERBOSE
    printf("Finish counting\n");
    std::fflush(stdout); 
#endif

    double totalCountTime = (utility::timer() - timeStart);
    printf("\nTime for count per iter: %9.6lf seconds\n", totalCountTime/_itr_num);
    std::fflush(stdout);

#ifdef VERBOSE
    printf("spmv ratio %f\% \n", 100*(_spmvTime/totalCountTime));
    std::fflush(stdout);
    printf("eMA ratio %f\% \n", 100*(_eMATime/totalCountTime));
    std::fflush(stdout);
    printf("Peak Mem Usage is : %9.6lf GB\n", _peakMemUsage);
    std::fflush(stdout);

    printf("SpMM time is : %f second; EMA time is: %f \n", _spmvElapsedTime/_itr_num, _fmaElapsedTime/_itr_num);
    std::fflush(stdout);

    printf("SpMM Memory bandwidth is : %f GBytes per second\n", (_spmvMemBytes*_itr_num)/_spmvElapsedTime);
    std::fflush(stdout);
    printf("FMA Memory bandwidth is : %f GBytes per second\n", (_fmaMemBytes*_itr_num)/_fmaElapsedTime);
    std::fflush(stdout);

    printf("Total Memory bandwidth is : %f GBytes per second\n", 
            ((_spmvMemBytes+_fmaMemBytes)*_itr_num)/(_spmvElapsedTime + _fmaElapsedTime));
    std::fflush(stdout);

    printf("SpMM Throughput is : %f Gflops per second\n", (_spmvFlops*_itr_num)/_spmvElapsedTime);
    std::fflush(stdout);
    printf("FMA Throughput is : %f Gflops per second\n", (_fmaFlops*_itr_num)/_fmaElapsedTime);
    std::fflush(stdout);
    printf("Total Throughput is : %f Gflops per second\n", 
            ((_spmvFlops+_fmaFlops)*_itr_num)/(_spmvElapsedTime+_fmaElapsedTime));
    std::fflush(stdout);

#endif

    // finish counting
    double finalCount = iterCount/(double)_itr_num;
    double probColorful = factorial(_color_num) / 
                ( factorial(_color_num - _templates->get_vert_num())*pow(_color_num, _templates->get_vert_num()));

    printf("Final raw count is %e\n", finalCount);
    std::fflush(stdout);

    printf("Prob is %f\n", probColorful);
    std::fflush(stdout);

    // int automoNum = 1;
    int automoNum = _calculate_automorphisms ? automorphismNum() : 1;  
    finalCount = floor(finalCount/(probColorful*(double)automoNum) + 0.5);

    printf("Final count is %e\n", finalCount);
    std::fflush(stdout);

    return finalCount;

}/*}}}*/

double CountMat::colorCounting()
{/*{{{*/

    colorInit();

    double countTotal = 0.0;
    // reset scaling flag
    _isScaled = 0;

    for (int s = _total_sub_num - 1; s >= 0; --s) {

        int subSize = _subtmp_array[s].get_vert_num();
        int mainIdx = div_tp.get_main_node_idx(s);
        int auxIdx = div_tp.get_aux_node_idx(s);

        _dTable.initSubTempTable(s, mainIdx, auxIdx);
        int* idxCombToCount = (indexer.getSubCToCount())[s]; 

        if (subSize == 1) {
            _dTable.countCurBottom(idxCombToCount, _colors_local);          
        }
        else
        {
            //non-bottom case
            if (_isPruned == 1)
            {
                if (_useSPMM == 1)
                {
                    countTotal = countNonBottomePrunedSPMM(s);
                }
                else
                   countTotal = countNonBottomePruned(s);
            }
            else
                countTotal = countNonBottomeOriginal(s);
        }

        // trace the peak mem usage
        double compute_mem = 0.0;
        process_mem_usage(compute_mem);
        compute_mem = compute_mem /(1024*1024);
        std::printf("Mem utilization compute step sub %d: %9.6lf GB, count val: %e\n", s, compute_mem, countTotal);
        std::fflush(stdout);
        _peakMemUsage = (compute_mem > _peakMemUsage) ? compute_mem : _peakMemUsage;

        if (mainIdx != DUMMY_VAL)
            _dTable.cleanSubTempTable(mainIdx, false);
        if (auxIdx != DUMMY_VAL)
            _dTable.cleanSubTempTable(auxIdx, false);
    }

    return countTotal;


}/*}}}*/

//reduce the num of SpMV
double CountMat::countNonBottomePruned(int subsId)
{/*{{{*/

    if (subsId == _vtuneStart)
    {
        // for vtune miami u15-2 subids==3 takes 103 secs
#ifdef VTUNE
        ofstream vtune_trigger;
        vtune_trigger.open("vtune-flag.txt");
        vtune_trigger << "Start training process and trigger vtune profiling.\n";
        vtune_trigger.close();
#endif
    }

    int subSize = _subtmp_array[subsId].get_vert_num();

    int idxMain = div_tp.get_main_node_idx(subsId);
    int idxAux = div_tp.get_aux_node_idx(subsId);

    int mainSize = indexer.getSubsSize()[idxMain];
    int auxSize = indexer.getSubsSize()[idxAux];
   
    int countCombNum = indexer.getCombTable()[_color_num][subSize];
    int splitCombNum = indexer.getCombTable()[subSize][mainSize];
    int auxTableLen = indexer.getCombTable()[_color_num][auxSize];

#ifdef VERBOSE
    printf("Finish init sub templte %d, vert: %d, comb: %d, splitNum: %d, spmv times: %d, isScaled: %d\n", subsId, subSize, 
            countCombNum, splitCombNum, auxTableLen, _isScaled);
    std::fflush(stdout); 
#endif

    double countSum = 0.0;
    double subSum = 0.0;
    int** mainSplitLocal = (indexer.getSplitToCountTable())[0][subsId]; 
    int** auxSplitLocal = (indexer.getSplitToCountTable())[1][subsId]; 
    int* combToCountLocal = (indexer.getCombToCountTable())[subsId];

    double* bufLastSub = nullptr;
    float* objArray = nullptr;

    if (subsId == 0)
    {
#ifdef __INTEL_COMPILER
      bufLastSub = (double*) _mm_malloc(_vert_num*sizeof(double), 64); 
#else
      bufLastSub = (double*) aligned_alloc(64, _vert_num*sizeof(double)); 
#endif

#pragma omp parallel for num_threads(omp_get_max_threads())
      for (int i = 0; i < _vert_num; ++i) {
          bufLastSub[i] = 0;
      }

    }

#ifdef VERBOSE
    double startTime = utility::timer();
    double startTimeComp = 0.0;
    double eltSpmv = 0.0;
    double eltMul = 0.0;
#endif

    double spmvStart = 0.0;
    double fmaStart = 0.0;
// first the precompute of SpMV results and have a in-place storage
#ifdef VERBOSE
   startTimeComp = utility::timer(); 
#endif

   if (_graph != nullptr && _graph->useMKL())
   {
       // setup hint for mkl spmv
       _graph->SpMVMKLHint(auxTableLen);
   }

   for (int i = 0; i < auxTableLen; ++i) {

       float* auxObjArray = _dTable.getAuxArray(i);

#ifdef VERBOSE
       spmvStart = utility::timer();
#endif

       if (_graph != nullptr)
       {

           if (_graph->useMKL())
           {
               //set up hint and optimize
               _graph->SpMVMKL(auxObjArray, _bufVec, _thd_num);
           }
           else
           {
               _graph->SpMVNaiveFull(auxObjArray, _bufVec, _thd_num); 
           }
       }
       else
       {
           // use CSC-Split SpMV
           // clear _bufVec
#pragma omp parallel for num_threads(omp_get_max_threads())
           for (int j = 0; j < _vert_num; ++j) {
               _bufVec[j] = 0.0;    
           }

           _graphCSC->spmvNaiveSplit(auxObjArray, _bufVec, _thd_num);
       }
           
#ifdef VERBOSE
       _spmvElapsedTime += (utility::timer() - spmvStart);
#endif

       // check the size of auxArray
       if (auxSize > 1)
          std::memcpy(auxObjArray, _bufVec, _vert_num*sizeof(float));
       else
          std::memcpy(_bufVecLeaf[i], _bufVec, _vert_num*sizeof(float));
   }
#ifdef VERBOSE
   eltSpmv += (utility::timer() - startTimeComp); 
#endif   

#ifdef VERBOSE
   startTimeComp = utility::timer(); 
#endif
// a second part only involves element-wise multiplication and updating
    for(int i=0; i<countCombNum; i++)
    {
        int combIdx = combToCountLocal[i];

// #ifdef VERBOSE
//         printf("Sub: %d, comb: %d, combIdx: %d\n", subsId, i, combIdx);
//         std::fflush(stdout);
// #endif

        if (subsId > 0)
            objArray = _dTable.getCurTableArray(combIdx);

        for (int j = 0; j < splitCombNum; ++j) {

            int mainIdx = mainSplitLocal[i][j];
            int auxIdx = auxSplitLocal[i][j];

// #ifdef VERBOSE
//         printf("Sub: %d, mainIdx: %d, auxIdx: %d\n", subsId, mainIdx, auxIdx);
//         std::fflush(stdout);
// #endif

            // already pre-computed by SpMV
            float* auxArraySelect = nullptr;
            if (auxSize > 1)
                auxArraySelect = _dTable.getAuxArray(auxIdx);
            else
                auxArraySelect = _bufVecLeaf[auxIdx];

            // element-wise mul 
            float* mainArraySelect = _dTable.getMainArray(mainIdx);

            #ifdef VERBOSE
                fmaStart = utility::timer();
            #endif

            if (subsId > 0)
            {
                if (_isScaled == 0)
                    _dTable.arrayWiseFMAScale(objArray, auxArraySelect, mainArraySelect, 1.0e-12);
                else
                {
                    _dTable.arrayWiseFMAAVX(objArray, auxArraySelect, mainArraySelect);
                    // _dTable.arrayWiseFMA(objArray, auxArraySelect, mainArraySelect);
                }
            }
            else
            {
                // the last scale use 
                _dTable.arrayWiseFMALast(bufLastSub, auxArraySelect, mainArraySelect);
            }

            #ifdef VERBOSE
               _fmaElapsedTime += (utility::timer() - fmaStart); 
            #endif

        }

        // if (subsId > 0)
        //     subSum += sumVec(objArray, _vert_num);

    }
#ifdef VERBOSE
   eltMul += (utility::timer() - startTimeComp); 
#endif

    _isScaled = 1;

    if (subsId == 0)
    {
        // sum the vals from bufLastSub  
        for (int k = 0; k < _vert_num; ++k) {
            countSum += bufLastSub[k];
        }

        // to recover the scale down process
        if (_isScaled == 1)
            countSum *= 1.0e+12;

#ifdef __INTEL_COMPILER
        _mm_free(bufLastSub);
#else
        free(bufLastSub);
#endif
    }

#ifdef VERBOSE
    double subsTime = (utility::timer() - startTime);
    printf("Sub %d, counting time %f, Spmv time %f: ratio: %f\%,  Mul time %f: ratio: %f\% \n", subsId, subsTime, eltSpmv, 100*(eltSpmv/subsTime), eltMul, 100*(eltMul/subsTime));
    _spmvTime += eltSpmv;
    _eMATime += eltMul;
    // printf("Sub %d, counting val %e\n", subsId, subSum);
    // std::fflush(stdout); 
#endif

// #ifdef VERBOSE
//     printf("Sub %d, NonBottom raw count %e\n", subsId, countSum);
//     std::fflush(stdout); 
// #endif

    return countSum;

}/*}}}*/

double CountMat::countNonBottomePrunedSPMM(int subsId)
{/*{{{*/

    if (subsId == _vtuneStart)
    {
        // for vtune miami u15-2 subids==3 takes 103 secs
#ifdef VTUNE
        ofstream vtune_trigger;
        vtune_trigger.open("vtune-flag.txt");
        vtune_trigger << "Start training process and trigger vtune profiling.\n";
        vtune_trigger.close();
#endif
    }

    int subSize = _subtmp_array[subsId].get_vert_num();

    int idxMain = div_tp.get_main_node_idx(subsId);
    int idxAux = div_tp.get_aux_node_idx(subsId);

    int mainSize = indexer.getSubsSize()[idxMain];
    int auxSize = indexer.getSubsSize()[idxAux];
   
    int countCombNum = indexer.getCombTable()[_color_num][subSize];
    int splitCombNum = indexer.getCombTable()[subSize][mainSize];
    int auxTableLen = indexer.getCombTable()[_color_num][auxSize];

#ifdef VERBOSE
    printf("Finish init sub templte %d, vert: %d, comb: %d, splitNum: %d, isScaled: %d\n", subsId, subSize, 
            countCombNum, splitCombNum, _isScaled);
    std::fflush(stdout); 
#endif

    double countSum = 0.0;
    double subSum = 0.0;
    int** mainSplitLocal = (indexer.getSplitToCountTable())[0][subsId]; 
    int** auxSplitLocal = (indexer.getSplitToCountTable())[1][subsId]; 
    int* combToCountLocal = (indexer.getCombToCountTable())[subsId];

    double* bufLastSub = nullptr;
    float* objArray = nullptr;

    if (subsId == 0)
    {
#ifdef __INTEL_COMPILER
      bufLastSub = (double*) _mm_malloc(_vert_num*sizeof(double), 64); 
#else
      bufLastSub = (double*) aligned_alloc(64, _vert_num*sizeof(double)); 
#endif

#pragma omp parallel for num_threads(omp_get_max_threads())
      for (int i = 0; i < _vert_num; ++i) {
          bufLastSub[i] = 0;
      }

    }

#ifdef VERBOSE
    double startTime = utility::timer();
    double startTimeComp = 0.0;
    double eltSpmv = 0.0;
    double eltMul = 0.0;
#endif
    double spmvStart = 0.0;
    double fmaStart = 0.0;

// first the precompute of SPMM results and have a in-place storage
#ifdef VERBOSE
   startTimeComp = utility::timer(); 
#endif

   // ---- start of SpMM impl -------
   if (_graph != nullptr) 
   {

#ifndef NEC
       // CSR MKL SpMM implementation 
       int batchNum = (auxTableLen + _bufMatCols - 1)/(_bufMatCols);
       int colStart = 0;

       char transa = 'n';
       MKL_INT m = _vert_num;
       MKL_INT n = 0;
       MKL_INT k = _vert_num;

       float alpha = 1.0;
       float beta = 0.0;

       char matdescra[5];
       matdescra[0] = 'g';
       matdescra[3] = 'f'; /*one-based indexing is used*/

       float* csrVals = _graph->getNNZVal();
       int* csrRowIdx = _graph->getIndexRow();
       int* csrColIdx = _graph->getIndexCol();



       mkl_set_num_threads(_thd_num);
       for (int i = 0; i < batchNum; ++i) 
       {

           int batchSize = (i < batchNum -1) ? (_bufMatCols) : (auxTableLen - _bufMatCols*(batchNum-1));
           n = batchSize;

#ifdef VERBOSE
       spmvStart = utility::timer();
#endif
           // invoke the mkl scsrmm kernel
           mkl_scsrmm(&transa, &m, &n, &k, &alpha, matdescra, csrVals, csrColIdx, csrRowIdx, &(csrRowIdx[1]), _dTable.getAuxArray(colStart), &k, &beta, _bufMatY, &k);

#ifdef VERBOSE
       _spmvElapsedTime += (utility::timer() - spmvStart);
#endif

           // copy columns from _bufMatY
           if (auxSize > 1)
           {
               std::memcpy(_dTable.getAuxArray(colStart), _bufMatY, _vert_num*batchSize*sizeof(float));
           }
           else
           {
               std::memcpy(_bufVecLeaf[colStart], _bufMatY, _vert_num*batchSize*sizeof(float));
           }

           // increase colStart;
           colStart += batchSize;
       }

#endif
   }
   else
   {
       // CSC-Split SpMM impl
       int batchNum = (auxTableLen + _bufMatCols - 1)/(_bufMatCols);
       int colStart = 0;

       for (int i = 0; i < batchNum; ++i) 
       {
           int batchSize = (i < batchNum -1) ? (_bufMatCols) : (auxTableLen - _bufMatCols*(batchNum-1));

           valType* xInput = _dTable.getAuxArray(colStart);

           // convert data structre from column-majored to row-majored
#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int j = 0; j <_vert_num; ++j) 
    {
        for (int k = 0; k < batchSize; ++k) {
            _bufMatX[j*batchSize+k] = xInput[k*_vert_num+j];
        }
    }
           // clean yOuput
#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int j = 0; j < _vert_num*batchSize; ++j) {
        _bufMatY[j] = 0.0; 
    }
           
#ifdef VERBOSE
       spmvStart = utility::timer();
#endif
           // invoke the spmm kernel
           _graphCSC->spmmSplit(_bufMatX, _bufMatY, batchSize, _thd_num);

#ifdef VERBOSE
       _spmvElapsedTime += (utility::timer() - spmvStart);
#endif

           // convert data structure back to column-majored
           valType* yOutput = (auxSize > 1) ? _dTable.getAuxArray(colStart) : _bufVecLeaf[colStart];

#pragma omp parallel for num_threads(omp_get_max_threads())
    for (int j = 0; j < _vert_num; ++j) {
        for (int k = 0; k < batchSize; ++k) {
            yOutput[k*_vert_num+j] = _bufMatY[j*batchSize+k];
        }
    }

           // increase colStart;
           colStart += batchSize;
       }
   }

#ifdef VERBOSE
   eltSpmv += (utility::timer() - startTimeComp); 
#endif   

#ifdef VERBOSE
   startTimeComp = utility::timer(); 
#endif

// a second part only involves element-wise multiplication and updating
    for(int i=0; i<countCombNum; i++)
    {
        int combIdx = combToCountLocal[i];

// #ifdef VERBOSE
//         printf("Sub: %d, comb: %d, combIdx: %d\n", subsId, i, combIdx);
//         std::fflush(stdout);
// #endif

        if (subsId > 0)
            objArray = _dTable.getCurTableArray(combIdx);

        for (int j = 0; j < splitCombNum; ++j) {

            int mainIdx = mainSplitLocal[i][j];
            int auxIdx = auxSplitLocal[i][j];

// #ifdef VERBOSE
//         printf("Sub: %d, mainIdx: %d, auxIdx: %d\n", subsId, mainIdx, auxIdx);
//         std::fflush(stdout);
// #endif

            // already pre-computed by SpMV
            float* auxArraySelect = nullptr;
            if (auxSize > 1)
                auxArraySelect = _dTable.getAuxArray(auxIdx);
            else
                auxArraySelect = _bufVecLeaf[auxIdx];

            // element-wise mul 
            float* mainArraySelect = _dTable.getMainArray(mainIdx);

#ifdef VERBOSE
            fmaStart = utility::timer();
#endif

            if (subsId > 0)
            {
                if (_isScaled == 0)
                    _dTable.arrayWiseFMAScale(objArray, auxArraySelect, mainArraySelect, 1.0e-12);
                else
                {
                    _dTable.arrayWiseFMAAVX(objArray, auxArraySelect, mainArraySelect);
                    // _dTable.arrayWiseFMA(objArray, auxArraySelect, mainArraySelect);

                }
            }
            else
            {
                // the last scale use 
                _dTable.arrayWiseFMALast(bufLastSub, auxArraySelect, mainArraySelect);
            }

#ifdef VERBOSE
            _fmaElapsedTime += (utility::timer() - fmaStart); 
#endif

        }

        // if (subsId > 0)
        //     subSum += sumVec(objArray, _vert_num);

    }
#ifdef VERBOSE
   eltMul += (utility::timer() - startTimeComp); 
#endif

    _isScaled = 1;

    if (subsId == 0)
    {
        // sum the vals from bufLastSub  
        for (int k = 0; k < _vert_num; ++k) {
            countSum += bufLastSub[k];
        }

        // to recover the scale down process
        if (_isScaled == 1)
            countSum *= 1.0e+12;

#ifdef __INTEL_COMPILER
        _mm_free(bufLastSub);
#else
        free(bufLastSub);
#endif
    }

#ifdef VERBOSE
    double subsTime = (utility::timer() - startTime);
    printf("Sub %d, counting time %f, Spmv time %f: ratio: %f\%,  Mul time %f: ratio: %f\% \n", subsId, subsTime, eltSpmv, 100*(eltSpmv/subsTime), eltMul, 100*(eltMul/subsTime));
    _spmvTime += eltSpmv;
    _eMATime += eltMul;
    // printf("Sub %d, counting val %e\n", subsId, subSum);
    // std::fflush(stdout); 
#endif

// #ifdef VERBOSE
//     printf("Sub %d, NonBottom raw count %e\n", subsId, countSum);
//     std::fflush(stdout); 
// #endif

    return countSum;

}/*}}}*/

double CountMat::countNonBottomeOriginal(int subsId)
{/*{{{*/

    if (subsId == _vtuneStart)
    {
        // for vtune
#ifdef VTUNE
        ofstream vtune_trigger;
        vtune_trigger.open("vtune-flag.txt");
        vtune_trigger << "Start training process and trigger vtune profiling.\n";
        vtune_trigger.close();
#endif
    }

    int subSize = _subtmp_array[subsId].get_vert_num();

    int idxMain = div_tp.get_main_node_idx(subsId);
    int mainSize = indexer.getSubsSize()[idxMain];

    int countCombNum = indexer.getCombTable()[_color_num][subSize];
    int splitCombNum = indexer.getCombTable()[subSize][mainSize];

#ifdef VERBOSE
    printf("Finish init sub templte %d, vert: %d, comb: %d, splitNum: %d\n", subsId, subSize, 
            countCombNum, splitCombNum);
    std::fflush(stdout); 
#endif

    double countSum = 0.0;
    int** mainSplitLocal = (indexer.getSplitToCountTable())[0][subsId]; 
    int** auxSplitLocal = (indexer.getSplitToCountTable())[1][subsId]; 
    int* combToCountLocal = (indexer.getCombToCountTable())[subsId];

    float* bufLastSub = nullptr;
    float* objArray = nullptr;

    if (subsId == 0)
    {
#ifdef __INTEL_COMPILER
      bufLastSub = (float*) _mm_malloc(_vert_num*sizeof(float), 64); 
#else
      bufLastSub = (float*) aligned_alloc(64, _vert_num*sizeof(float)); 
#endif
#pragma omp parallel for num_threads(omp_get_max_threads())
      for (int i = 0; i < _vert_num; ++i) {
          bufLastSub[i] = 0;
      }
      // std::memset(bufLastSub, 0, _vert_num*sizeof(float)); 

    }

#ifdef VERBOSE
    double startTime = utility::timer();
    double startTimeComp = 0.0;
    double eltSpmv = 0.0;
    double eltMul = 0.0;
#endif
   
    for(int i=0; i<countCombNum; i++)
    {
        int combIdx = combToCountLocal[i];

        if (subsId == 0)
            objArray = bufLastSub;
        else
            objArray = _dTable.getCurTableArray(combIdx);

        for (int j = 0; j < splitCombNum; ++j) {

            int mainIdx = mainSplitLocal[i][j];
            int auxIdx = auxSplitLocal[i][j];

            float* auxArraySelect = _dTable.getAuxArray(auxIdx);
            // spmv
#ifdef VERBOSE
            startTimeComp = utility::timer();
#endif
            if (_graph != nullptr)
                _graph->SpMVNaiveFull(auxArraySelect, _bufVec, _thd_num);
            else
                _graphCSC->spmvNaiveSplit(auxArraySelect, _bufVec, _thd_num);

#ifdef VERBOSE
            eltSpmv += (utility::timer() - startTimeComp);
#endif

            // element-wise mul 
            float* mainArraySelect = _dTable.getMainArray(mainIdx);
#ifdef VERBOSE
            startTimeComp = utility::timer();
#endif

            _dTable.arrayWiseFMANaive(objArray, _bufVec, mainArraySelect);
#ifdef VERBOSE
            eltMul += (utility::timer() - startTimeComp);
#endif

        }
    }

    if (subsId == 0)
    {
        // sum the vals from bufLastSub  
        for (int k = 0; k < _vert_num; ++k) {
            countSum += bufLastSub[k];
        }
#ifdef __INTEL_COMPILER
        _mm_free(bufLastSub);
#else
        free(bufLastSub);
#endif
    }

#ifdef VERBOSE
    printf("Sub %d, counting time %f, Spmv time %f, Mul time %f \n", subsId, (utility::timer() - startTime), eltSpmv, eltMul);
    // printf("Sub %d, counting time %f\n", subsId, (utility::timer() - startTime));
    std::fflush(stdout); 
#endif

#ifdef VERBOSE
    printf("Sub %d, NonBottom raw count %f\n", subsId, countSum);
    std::fflush(stdout); 
#endif

    return countSum;

}/*}}}*/

void CountMat::colorInit()
{
#pragma omp parallel
    {
        // each thread has a seed for ranodm number generation
        srand(time(0)+ omp_get_thread_num());

#pragma omp for
        for (int i = 0; i < _vert_num; ++i) {
            _colors_local[i] = (rand()%_color_num);
        }
    }
}

int CountMat::factorial(int n)
{
    if (n <= 1)
        return 1;
    else
        return (n*factorial(n-1));
}


double CountMat::sumVec(valType* input, idxType len)
{
    double sum = 0.0;
#pragma omp parallel for reduction(+:sum) 
    for (idxType i = 0; i < len; ++i) {
        sum += input[i];
    }

    return sum;
}

void CountMat::scaleVec(valType* input, idxType len, double scale)
{
#pragma omp parallel for  
    for (idxType i = 0; i < len; ++i) {
        double tmp = scale*input[i];
        input[i] = (float)tmp;
    }

}

void CountMat::process_mem_usage(double& resident_set)
{
    resident_set = 0.0;

    FILE *fp;
    long vmrss;
    int BUFFERSIZE=80;
    char *buf= new char[85];
    if((fp = fopen("/proc/self/status","r")))
    {
        while(fgets(buf, BUFFERSIZE, fp) != NULL)
        {
            if(strstr(buf, "VmRSS") != NULL)
            {
                if (sscanf(buf, "%*s %ld", &vmrss) == 1){
                    // printf("VmSize is %dKB\n", vmrss);
                    resident_set = (double)vmrss;
                }
            }
        }
    }

    fclose(fp);
    delete[] buf;
}

void CountMat::printSubTemps()
{
    //check the sub vert num
    // debug
    for(int s=0;s<_total_sub_num;s++)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1)
        {
            int main_leaf = div_tp.get_main_node_vert_num(s);
            int aux_leaf = div_tp.get_aux_node_vert_num(s);
            printf("Temp Sizes: Self %d, main: %d, aux: %d\n", vert_self, main_leaf, aux_leaf);
            std::fflush(stdout);
            assert((main_leaf + aux_leaf == vert_self));
        }
    }
}

void CountMat::estimatePeakMemUsage()
{

    idxType n = 0; 
    idxType nnz = 0; 

    if (_graph != nullptr)
    {
        n = _graph->getNumVertices();
        nnz = _graph->getNNZ();
    }
    else
    {
        n = _graphCSC->getNumVertices();
        nnz = _graphCSC->getNNZ();
    }

    double peakMem = 0.0;
    double memSub = 0.0;
    // memery (GB) usaed by each idx (column)
    double memPerIndx = ((double)_vert_num*4.0)/1024/1024/1024;

    // bufvec, color_inital, bufVecY, Bufleaf 
    memSub += (2 + _bufMatCols + _color_num)*memPerIndx;

    for(int s=_total_sub_num-1;s>0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1)
        {
            memSub += (_dTable.getTableLen(s)*memPerIndx);
            peakMem = (memSub > peakMem) ? memSub : peakMem;

            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);
            if (_subtmp_array[idxMain].get_vert_num() > 1)
                memSub -= (_dTable.getTableLen(idxMain)*memPerIndx);

            if (_subtmp_array[idxAux].get_vert_num() > 1)
                memSub -= (_dTable.getTableLen(idxAux)*memPerIndx);
        }
    }

    // add the input graph mem usage
    peakMem += ((double)(n + nnz*2)*4.0/1024/1024/1024); 

    printf("Peak memory usage estimated : %f GB \n", peakMem);
    std::fflush(stdout);
}

double CountMat::estimateMemCommPGBSC()
{

    idxType n = 0; 
    idxType nnz = 0; 

    if (_graph != nullptr)
    {
        n = _graph->getNumVertices();
        nnz = _graph->getNNZ();
    }
    else
    {
        n = _graphCSC->getNumVertices();
        nnz = _graphCSC->getNNZ();
    }

    double commBytesTotal = 0.0;
    // AB = C
    // nnz row id, nnz col id, batch of 16 (nnz 16 + nnz 16 write)
    double bytesSpmvPer = sizeof(float)*(2*(double)nnz) + sizeof(int)*((double)2*nnz/16);
    // z += x*y
    // read x, y, z 
    double bytesFMAPer = sizeof(float)*(n*3);

    for(int s=_total_sub_num-1;s>=0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1) {
            
            // spmv part
            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);       
            if (_subtmp_array[idxAux].get_vert_num() > 1)
            {
                _spmvMemBytes += (bytesSpmvPer*_dTable.getTableLen(idxAux));
            }

            // FMA part
            _fmaMemBytes += (_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())*
                    bytesFMAPer);
        }
    }

    _spmvMemBytes /= (1024*1024*1024);
    _fmaMemBytes /= (1024*1024*1024);

    commBytesTotal = _spmvMemBytes + _fmaMemBytes;

    printf("Comm Bytes estimated PGBSC : SpMV %f GBytes, FMA %f GBytes \n", _spmvMemBytes, _fmaMemBytes);
    std::fflush(stdout);

    return commBytesTotal;
}

double CountMat::estimateMemCommFascia()
{

    idxType n = 0; 
    idxType nnz = 0; 

    if (_graph != nullptr)
    {
        n = _graph->getNumVertices();
        nnz = _graph->getNNZ();
    }
    else
    {
        n = _graphCSC->getNumVertices();
        nnz = _graphCSC->getNNZ();
    }

    double commBytesTotal = 0.0;
    double commBytesComb = sizeof(float)*(double)n + sizeof(int)*n;
    double commBytesSplit = sizeof(float)*(n+(double)nnz) + sizeof(int)*2*(double)n;

    // val: nnz passive + n active + n comb read/write + , Index: nnz colIdx, n rowIdx 
    // double bytpesByAux = sizeof(float)*(3*n) 
    //     + sizeof(int)*(nnz+n);

    for(int s=_total_sub_num-1;s>=0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1) {
            
            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);       
            
            commBytesTotal += (_dTable.getTableLen(s)*(commBytesComb));
            commBytesTotal += (_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())*(commBytesSplit));
        }
    }

    commBytesTotal /= (1024*1024*1024);
    printf("Comm Bytes Fascia estimated:  %f GBytes\n", commBytesTotal);
    std::fflush(stdout);

    return commBytesTotal;
}

double CountMat::estimateMemCommPrunedFascia()
{

    idxType n = 0; 
    idxType nnz = 0; 

    if (_graph != nullptr)
    {
        n = _graph->getNumVertices();
        nnz = _graph->getNNZ();
    }
    else
    {
        n = _graphCSC->getNumVertices();
        nnz = _graphCSC->getNNZ();
    }

    double commBytesTotal = 0.0;
    double commBytesPrune = sizeof(float)*(double)nnz;
    double commBytesComb = sizeof(float)*(double)n + sizeof(int)*n;
    double commBytesSplit = sizeof(float)*(2*(double)n) + sizeof(int)*2*(double)n;

    for(int s=_total_sub_num-1;s>=0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1) {
            
            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);       
     
            if (_subtmp_array[idxAux].get_vert_num() > 1)
            {
                commBytesTotal += (commBytesPrune*_dTable.getTableLen(idxAux));
            }       

            commBytesTotal += (_dTable.getTableLen(s)*(commBytesComb));
            commBytesTotal += (_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())*(commBytesSplit));
        }
    }

    commBytesTotal /= (1024*1024*1024);
    printf("Comm Bytes Pruned Fascia estimated:  %f GBytes\n", commBytesTotal);
    std::fflush(stdout);

    return commBytesTotal;
}

// estimate flops for pruned algorithm
// with non-touching nnz val SpMV
double CountMat::estimateFlopsPGBSC()
{
    idxType n = 0; 
    idxType nnz = 0; 

    if (_graph != nullptr)
    {
        n = _graph->getNumVertices();
        nnz = _graph->getNNZ();
    }
    else
    {
        n = _graphCSC->getNumVertices();
        nnz = _graphCSC->getNNZ();
    }


    printf("|V| is: %d, |E| nnz is: %d \n", n , nnz );
    std::fflush(stdout);

    double flopsTotal = 0.0;
    double flopsSpmvPer = nnz; 
    // n mul + n add
    double flopsFMAPer = 2*(double)n;

    for(int s=_total_sub_num-1;s>=0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1) {
            
            // spmv part
            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);       
            if (_subtmp_array[idxAux].get_vert_num() > 1)
            {
                _spmvFlops += (flopsSpmvPer*_dTable.getTableLen(idxAux));
            }

            // FMA part
            _fmaFlops += (_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())*
                    flopsFMAPer);
        }
    }

    _spmvFlops /= (1024*1024*1024);
    _fmaFlops /= (1024*1024*1024);

    flopsTotal = _spmvFlops + _fmaFlops;

    printf("Flops PGBSC estimated : SpMV %f Gflop, FMA %f Gflop \n", _spmvFlops, _fmaFlops);
    std::fflush(stdout);

    return flopsTotal;
}

// for flops with pruned fascia
double CountMat::estimateFlopsPrunedFascia()
{
    idxType n = 0; 
    idxType nnz = 0; 

    if (_graph != nullptr)
    {
        n = _graph->getNumVertices();
        nnz = _graph->getNNZ();
    }
    else
    {
        n = _graphCSC->getNumVertices();
        nnz = _graphCSC->getNNZ();
    }


    printf("|V| is: %d, |E| nnz is: %d \n", n , nnz );
    std::fflush(stdout);

    double flopsTotal = 0.0;
    // nnz summation + n mul + n addition
    double flopsPrunedPer = nnz;
    double flopsPer = 2*n;

    for(int s=_total_sub_num-1;s>=0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1) {
            
            // spmv part
            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);       
            if (_subtmp_array[idxAux].get_vert_num() > 1)
            {
                flopsTotal += (flopsPrunedPer*_dTable.getTableLen(idxAux));
            }           

            flopsTotal += (_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())*
                    flopsPer);
        }
    }

    flopsTotal /= (1024*1024*1024);

    printf("Flops estimated Pruned Fascia: %f Gflop\n", flopsTotal);
    std::fflush(stdout);

    return flopsTotal;

}

void CountMat::degreeDistribution()
{
    // output the distribution of input graph vertices
    idxType* degList = (_graph != nullptr) ? _graph->getDegList() : _graphCSC->getDegList();
    // find the max degree
    long maxDeg = 0;
    for (int i = 0; i < _vert_num; ++i) {
       maxDeg = (degList[i] > maxDeg) ? degList[i] : maxDeg; 
    }

    std::cout<<"maxDeg: "<<maxDeg<<std::endl;
    std::vector<idxType> degSort(maxDeg+1, 0);
    for (int i = 0; i < _vert_num; ++i) {
       degSort[degList[i]]++;
    }
    //
    // output nonzero value to a file
    ofstream output_file("degDistri.data");

    for (int i = 1; i < degSort.size(); ++i) {
        if (degSort[i] > 0) 
            output_file<<i<<" "<<degSort[i]<<std::endl;
    }

    output_file.close();

}

double CountMat::estimateFlopsFascia()
{

    idxType n = 0; 
    idxType nnz = 0; 

    if (_graph != nullptr)
    {
        n = _graph->getNumVertices();
        nnz = _graph->getNNZ();
    }
    else
    {
        n = _graphCSC->getNumVertices();
        nnz = _graphCSC->getNNZ();
    }

    printf("|V| is: %d, |E| nnz is: %d \n", n, nnz);
    std::fflush(stdout);

    double flopsTotal = 0.0;
    // nnz summation + n mul + n addition
    double flopsPer = (double)nnz + 2*(double)n;

    for(int s=_total_sub_num-1;s>=0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1) {
            
            // spmv part
            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);       
            
            flopsTotal += (_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())*
                    flopsPer);
        }
    }

    flopsTotal /= (1024*1024*1024);

    printf("Flops estimated fascia: %f Gflop\n", flopsTotal);
    std::fflush(stdout);

    return flopsTotal;
}

double CountMat::estimateTemplate()
{
    double workloadNNZ = 0.0; 
    double workloadN = 0.0;

    // // Ax = y
    // // access A + access x + write to y + rowidx + colidx
    // double bytesSpmvPer = sizeof(float)*(2*_graph->getNNZ() + _graph->getNumVertices()) + sizeof(int)*(_graph->getNNZ()
    //         + _graph->getNumVertices());
    //
    // // z += x*y
    // // read x, y, z and write to z
    // double bytesFMAPer = sizeof(float)*(_graph->getNumVertices()*4);

    double memloadNNZ = 0.0;
    double memloadN = 0.0;

    double perSpMV = 2;
    double pereMA = 2;

    for(int s=_total_sub_num-1;s>=0;s--)
    {
        int vert_self = _subtmp_array[s].get_vert_num();
        if (vert_self > 1) {
            
            // spmv part
            int idxMain = div_tp.get_main_node_idx(s);
            int idxAux = div_tp.get_aux_node_idx(s);       
            if (_subtmp_array[idxAux].get_vert_num() > 1)
            {
                 workloadNNZ += (perSpMV*_dTable.getTableLen(idxAux));
                 memloadNNZ += (12*_dTable.getTableLen(idxAux)); 
                 memloadN += (8*_dTable.getTableLen(idxAux)); 
            }

            // FMA part
            workloadN += (_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())*
                    pereMA);

            memloadN += (16*_dTable.getTableLen(s)*indexer.comb_calc(vert_self, _subtmp_array[idxAux].get_vert_num())); 

        }
    }

    printf("Workload N: %f, NNZ: %f \n", workloadN, workloadNNZ);
    printf("Memload N: %f, NNZ: %f \n", memloadN, memloadNNZ);
    std::fflush(stdout);

}

int CountMat::automorphismNum()
{
    std::vector<int> mappingID;
    std::vector<int> restID;

    for (int i = 0; i < _templates->get_vert_num(); ++i) {
        restID.push_back(i);
    }

    return calcAutomorphismRecursive((*_templates), mappingID, restID);
}

int CountMat::calcAutomorphismZero(Graph& t, std::vector<int>& mappingID)
{
    for (int i = 0; i < mappingID.size(); ++i) {
       if (t.get_out_deg(i) != t.get_out_deg(mappingID[i])) 
           return 0;
       else
       {
           int* adjList = t.get_adj_list(i);
           int* adjListMap = t.get_adj_list(mappingID[i]);
           int end = t.get_out_deg(i);

           bool* isMatch = new bool[end];
           for (int j = 0; j < end; ++j) {
               isMatch[j] = false;
               int u = adjList[j];
               for (int k = 0; k < end; ++k) {
                   int u_map = adjListMap[k];
                   if (u == mappingID[u_map])
                       isMatch[j] = true;
               }
               
           }

           for (int k = 0; k < end; ++k) {
              
               if (!isMatch[k])
                   return 0;
           }

       }
        
    }
    return 1;
}

int CountMat::calcAutomorphismRecursive(Graph& t, std::vector<int>& mappingID, std::vector<int>& restID)
{
    int count = 0;
    if (!restID.size())
    {
        return calcAutomorphismZero(t, mappingID); 
    }
    else
    {
        for (int i = 0; i < restID.size(); ++i) {
            mappingID.push_back(restID[i]);
            std::vector<int> newRestID;
            for (int j = 0; j < restID.size(); ++j) {
                if (i!=j)
                    newRestID.push_back(restID[j]);
                
            }

            count += calcAutomorphismRecursive(t, mappingID, newRestID);
            newRestID.clear();
            mappingID.pop_back();
        }

    }

    return count;

}



