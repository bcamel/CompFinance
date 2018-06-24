
/*
Written by Antoine Savine in 2018

This code is the strict IP of Antoine Savine

License to use and alter this code for personal and commercial applications
is freely granted to any person or company who purchased a copy of the book

Modern Computational Finance: AAD and Parallel Simulations
Antoine Savine
Wiley, 2018

As long as this comment is preserved at the top of the file
*/

#pragma once

#include "AAD.h"

#include <vector>
#include <memory>
#include <algorithm>
#include <numeric>

using namespace std;

#include "matrix.h"
#include "threadPool.h"

using Time = double;
extern Time systemTime;

//  simulData = definition 
//      of what data must be simulated
struct simulData
{
    vector<Time>    forwardMats;
    vector<Time>    discountMats;
};

//  scenario = simulated value
//      of data on a given event date
template <class T>
struct scenario
{
    T           numeraire;
    vector<T>   forwards;
    vector<T>   discounts;

    //  Allocate given simulData
    void allocate(const simulData& data)
    {
        forwards.resize(data.forwardMats.size());
        discounts.resize(data.discountMats.size());
    }
};

template <class T>
inline void allocatePath(const vector<simulData>& dataline, vector<scenario<T>>& path)
{
    path.resize(dataline.size());
    for (size_t i = 0; i < dataline.size(); ++i)
    {
        path[i].allocate(dataline[i]);
    }
}

template <class T>
class Product
{
public:

    //  Access to the product timeline
    //      along with the necessary simulData
    virtual const vector<Time>& timeline() const = 0;
    virtual const vector<simulData>& dataline() const = 0;

    //  Number of payoffs in the product, 1 by default
    virtual size_t numPayoffs() const
    {
        return 1;
    }

    //  Compute payoffs given a path (on the product timeline)
    virtual void payoffs(
        //  path, one entry per time step (on the product timeline)
        const vector<scenario<T>>&  path,     
        //  pre-allocated space for resulting payoffs
        vector<T>&                  payoffs)       
            const = 0;

    virtual unique_ptr<Product<T>> clone() const = 0;

    virtual ~Product() {}
};

template <class T>
class Model
{
public:

    //  Initialize with product timeline
    virtual void allocate(const vector<Time>& prdTimeline, const vector<simulData>& prdDataline) = 0;
    virtual void init(const vector<Time>& prdTimeline, const vector<simulData>& prdDataline) = 0;

    //  Access to the MC dimension
    virtual size_t simDim() const = 0;

    //  Generate a path consuming a vector[simDim()] of independent Gaussians
    //  The path vector filled by the function must be pre-allocated
    virtual void generatePath(const vector<double>& gaussVec, vector<scenario<T>>& path) const = 0;

    virtual unique_ptr<Model<T>> clone() const = 0;

    virtual ~Model() {}

    //  Access to all the model parameters and what they mean
    virtual const vector<T*>& parameters() = 0;
    virtual const vector<string>& parameterLabels() const = 0;

    //  Number of parameters
    size_t numParams() const
    {
        return const_cast<Model*>(this)->parameters().size();
    }

    //  Put parameters on tape, only valid for T = Number
    void putParametersOnTape()
    {
        putParametersOnTapeT<T>();
    }

private:

    //  If T not Number : do nothing
    template<class U> 
    void putParametersOnTapeT()
    {

    }

    //  If T = Number : put on tape
    template <>
    void putParametersOnTapeT<Number>()
    {
        for (Number* param : parameters()) param->putOnTape();
    }
};

class RNG
{
public:
    
    //  Initialise with dimension simDim
    virtual void init(const size_t simDim) = 0;

    //  Compute the next vector[simDim] of independent Gaussians
    //  The vector is filled by the function and must be pre-allocated
    virtual void nextG(vector<double>& gaussVec) = 0;

    virtual unique_ptr<RNG> clone() const = 0;

    virtual ~RNG() {}

    //  Access dimension
    virtual size_t simDim() const = 0;

    //  Skip ahead
    virtual void skipTo(const long b) = 0;
};

//	MC simulator: free function that conducts simulations 
//      and returns a matrix (as vector of vectors) (0..nPath-1 , 0..nPay-1) of payoffs 
inline vector<vector<double>> mcSimul(
    const Product<double>& prd,
    const Model<double>& mdl,
    const RNG& rng,			            
    const size_t nPath)                      
{
    //  Work with copies of the model and RNG
    //      which are modified when we set up the simulation
    //  Copies are OK at high level
    auto cMdl = mdl.clone();
    auto cRng = rng.clone();

    //	Allocate results
    const size_t nPay = prd.numPayoffs();
    vector<vector<double>> results(nPath, vector<double>(nPay));
    //  Init the simulation timeline
    cMdl->allocate(prd.timeline(), prd.dataline());
    cMdl->init(prd.timeline(), prd.dataline());              
    //  Init the RNG
    cRng->init(cMdl->simDim());                        
    //  Allocate Gaussian vector
    vector<double> gaussVec(cMdl->simDim());           
    //  Allocate path
    vector<scenario<double>> path;
    allocatePath(prd.dataline(), path);

    //	Iterate through paths	
    for (size_t i = 0; i<nPath; i++)
    {
        //  Next Gaussian vector, dimension D
        cRng->nextG(gaussVec);                        
        //  Generate path, consume Gaussian vector
        cMdl->generatePath(gaussVec, path);     
        //	Compute result
        prd.payoffs(path, results[i]);
    }

    return results;	//	C++11: move
}

#define BATCHSIZE 64
//	Parallel equivalent of mcSimul()
inline vector<vector<double>> mcParallelSimul(
    const Product<double>& prd,
    const Model<double>& mdl,
    const RNG& rng,
    const size_t nPath)
{
    auto cMdl = mdl.clone();

    const size_t nPay = prd.numPayoffs();
    vector<vector<double>> results(nPath, vector<double>(nPay));

    cMdl->allocate(prd.timeline(), prd.dataline());
    cMdl->init(prd.timeline(), prd.dataline());

    //  Allocate space for Gaussian vectors and paths, 
    //      one for each thread
    ThreadPool *pool = ThreadPool::getInstance();
    const size_t nThread = pool->numThreads();
    vector<vector<double>> gaussVecs(nThread+1);    //  +1 for main
    vector<vector<scenario<double>>> paths(nThread+1);
    for (auto& vec : gaussVecs) vec.resize(cMdl->simDim());
    for (auto& path : paths) allocatePath(prd.dataline(), path);
    
    //  One RNG per thread
    vector<unique_ptr<RNG>> rngs(nThread + 1);
    for (auto& random : rngs)
    {
        random = rng.clone();
        random->init(cMdl->simDim());
    }

    //  Reserve memory for futures
    vector<TaskHandle> futures;
    futures.reserve(nPath / BATCHSIZE + 1); 

    //  Start
    //  Same as mcSimul() except we send tasks to the pool 
    //  instead of executing them

    size_t firstPath = 0;
    size_t pathsLeft = nPath;
    while (pathsLeft > 0)
    {
        size_t pathsInTask = min<size_t>(pathsLeft, BATCHSIZE);

        futures.push_back( pool->spawnTask ( [&, firstPath, pathsInTask]()
        {
            //  Inside the parallel task, 
            //      pick the right pre-allocated vectors
            const size_t threadNum = pool->threadNum();
            vector<double>& gaussVec = gaussVecs[threadNum];
            vector<scenario<double>>& path = paths[threadNum];

            //  Get a RNG and position it correctly
            auto& random = rngs[threadNum];
            random->skipTo(firstPath);

            //  And conduct the simulations, exactly same as sequential
            bool antiPath = false;
            for (size_t i = 0; i < pathsInTask; i++)
            {
                //  Next Gaussian vector, dimension D
                random->nextG(gaussVec);
                //  Path
                cMdl->generatePath(gaussVec, path);       
                //  Payoff
                prd.payoffs(path, results[firstPath + i]);
            }

            //  Remember tasks must return bool
            return true;
        }));

        pathsLeft -= pathsInTask;
        firstPath += pathsInTask;
    }

    //  Wait and help
    for (auto& future : futures) pool->activeWait(future);

    return results;	//	C++11: move
}

//  AAD instrumentation of mcSimul()
//  returns the following results:

struct AADSimulResults
{
    AADSimulResults(const size_t nPath, const size_t nPay, const size_t nParam) :
        payoffs(nPath, vector<double>(nPay)),
        aggregated(nPath),
        risks(nParam)
    {}

    //  matrix(0..nPath - 1, 0..nPay - 1) of payoffs, same as mcSimul()
    vector<vector<double>>  payoffs;

    //  vector(0..nPath) of aggregated payoffs
    vector<double>          aggregated;

    //  vector(0..nParam - 1) of risk sensitivities
    //  of aggregated payoff, averaged over paths
    vector<double>          risks;
};

//  Default aggregator = 1st payoff = payoff[0]
const auto defaultAggregator = [](const vector<Number>& v) {return v[0]; };

template<class F = decltype(defaultAggregator)>
inline AADSimulResults
mcSimulAAD(
    const Product<Number>& prd,
    const Model<Number>& mdl,
    const RNG& rng,
    const size_t            nPath,
    const F&                aggFun = defaultAggregator)
{
    //  Work with copies of the model and RNG
    //      which are modified when we set up the simulation
    //  Copies are OK at high level
    auto cMdl = mdl.clone();
    auto cRng = rng.clone();

    //  AAD - 1
    //  Access to tape
    Tape& tape = *Number::tape;
    //  Rewind tape
    tape.rewind();
    //  Put parameters on tape
    //  note that also initializes all adjoints
    cMdl->putParametersOnTape();
    //  Init the simulation timeline
    //  CAREFUL: simulation timeline must be on tape
    //  Hence moved here
    cMdl->allocate(prd.timeline(), prd.dataline());
    cMdl->init(prd.timeline(), prd.dataline());
    //  Mark the tape straight after initialization
    tape.mark();
    //

    //  Init the RNG
    cRng->init(cMdl->simDim());                         
                                                        
    //  Dimensions
    const size_t nPay = prd.numPayoffs();
    const vector<Number*>& params = cMdl->parameters();
    const size_t nParam = params.size();
    
    //  Allocate workspace
    vector<Number> nPayoffs(nPay);
    //  Gaussian vector
    vector<double> gaussVec(cMdl->simDim());            
    //  Path
    vector<scenario<Number>> path;
    allocatePath(prd.dataline(), path);

    //  Results
    AADSimulResults results(nPath, nPay, nParam);

    //	Iterate through paths	
    for (size_t i = 0; i<nPath; i++)
    {
        //  AAD - 2
        //  Rewind tape to mark
        //  parameters stay on tape but the rest is wiped
        tape.rewindToMark();
        //

        //  Next Gaussian vector, dimension D
        cRng->nextG(gaussVec);
        //  Generate path, consume Gaussian vector
        cMdl->generatePath(gaussVec, path);     
        //	Compute result
        prd.payoffs(path, nPayoffs);
        Number result = aggFun(nPayoffs);

        //  AAD - 3
        //  Propagate adjoints
        result.propagateToMark();
        //

        //  Store results for the path
        results.aggregated[i] = convert<double>(result);
        convertCollection(nPayoffs.begin(), nPayoffs.end(), results.payoffs[i].begin());
    }

    //  AAD - 4
    //  Mark = limit between pre-calculations and path-wise operations
    //  Operations above mark have been propagated and accumulated
    //  We conduct one propagation mark to start
    Number::propagateMarkToStart();

    //  Pick sensitivities, summed over paths, and normalize
    transform(
        params.begin(), 
        params.end(), 
        results.risks.begin(), 
        [nPath](const Number* p) {return p->adjoint() / nPath; });
    
    //  Clear the tape
    tape.clear();

    return results;
}

//  Init model and out on tape
template<class T>
void initModel4ParallelAAD(
    //  Inputs
    const Product<T>&       prd,
    //  Cloned model, must have been allocated prior
    Model<T>&               clonedMdl)
{
    //  Access to tape
    Tape& tape = *Number::tape;
    //  Rewind tape
    tape.rewind();
    //  Put parameters on tape
    //  note that also initializes all adjoints
    clonedMdl.putParametersOnTape();
    //  Init the simulation timeline
    //  CAREFUL: simulation timeline must be on tape
    //  Hence moved here
    clonedMdl.init(prd.timeline(), prd.dataline());
    //  Mark the tape straight after parameters
    tape.mark();
    //
}

//  Parallel version of mcSimulAAD()
template<class F = decltype(defaultAggregator)>
inline AADSimulResults
mcParallelSimulAAD(
    const Product<Number>&  prd,
    const Model<Number>&    mdl,
    const RNG& rng,
    const size_t            nPath,
    const F&                aggFun = defaultAggregator)
{
    //  We need one of all these for each thread
    //  0: main thread
    //  1 to n : worker threads

    ThreadPool *pool = ThreadPool::getInstance();
    const size_t nThread = pool->numThreads();

    //  Allocate workspace

    const size_t nPay = prd.numPayoffs();
    const size_t nParam = mdl.numParams();

    //  One model clone per thread
    vector<unique_ptr<Model<Number>>> models(nThread + 1);
    for (auto& model : models)
    {
        model = mdl.clone();
        model->allocate(prd.timeline(), prd.dataline());
    }

    //  One scenario per thread
    vector<vector<scenario<Number>>> paths(nThread + 1);
    for (auto& path : paths)
    {
        allocatePath(prd.dataline(), path);
    }

    //  One vector of payoffs per thread
    vector<vector<Number>> payoffs(nThread + 1, vector<Number>(nPay));

    //  ~workspace

    //  Tapes for the worker threads
    //  The main thread has one of its own
    vector<Tape> tapes(nThread);

    //  Model initialized?
    vector<int> mdlInit(nThread + 1, false);

    //  Initialize main thread
    initModel4ParallelAAD(prd, *models[0]);

    //  Mark main thread as initialized
    mdlInit[0] = true;

    //  Init the RNGs, one pet thread
    //  One RNG per thread
    vector<unique_ptr<RNG>> rngs(nThread + 1);
    for (auto& random : rngs)
    {
        random = rng.clone();
        random->init(models[0]->simDim());
    }

    //  One Gaussian vector per thread
    vector<vector<double>> gaussVecs
        (nThread + 1, vector<double>(models[0]->simDim()));

    //  Allocate results
    AADSimulResults results(nPath, nPay, nParam);

    //  Reserve memory for futures
    vector<TaskHandle> futures;
    futures.reserve(nPath / BATCHSIZE + 1);

    //  Start
    //  Same as mcSimul() except we send tasks to the pool 
    //  instead of executing them

    size_t firstPath = 0;
    size_t pathsLeft = nPath;
    while (pathsLeft > 0)
    {
        size_t pathsInTask = min<size_t>(pathsLeft, BATCHSIZE);

        futures.push_back(pool->spawnTask([&, firstPath, pathsInTask]()
        {
            const size_t threadNum = pool->threadNum();

            //  Use this thread's tape
            //  Thread local magic: each thread its own pointer
            //  Note main thread = 0 is not reset
            if (threadNum > 0) Number::tape = &tapes[threadNum - 1];

            //  Initialize once on each thread
            if (!mdlInit[threadNum])
            {
                //  Initialize
                initModel4ParallelAAD(prd, *models[threadNum]);

                //  Mark as initialized
                mdlInit[threadNum] = true;
            }

            //  Get a RNG and position it correctly
            auto& random = rngs[threadNum];
            random->skipTo(firstPath);

            //  And conduct the simulations, exactly same as sequential
            for (size_t i = 0; i < pathsInTask; i++)
            {
                //  Rewind tape to mark
                //  Notice : this is the tape for the executing thread
                Number::tape->rewindToMark();

                //  Next Gaussian vector, dimension D
                random->nextG(gaussVecs[threadNum]);
                //  Path
                models[threadNum]->generatePath(
                    gaussVecs[threadNum], 
                    paths[threadNum]);
                //  Payoff
                prd.payoffs(paths[threadNum], payoffs[threadNum]);
                Number result = aggFun(payoffs[threadNum]);

                //  Propagate adjoints
                result.propagateToMark();

                //  Store results for the path
                results.aggregated[firstPath + i] = convert<double>(result);
                convertCollection(
                    payoffs[threadNum].begin(), 
                    payoffs[threadNum].end(),
                    results.payoffs[firstPath + i].begin());
            }

            //  Remember tasks must return bool
            return true;
        }));

        pathsLeft -= pathsInTask;
        firstPath += pathsInTask;
    }

    //  Wait and help
    for (auto& future : futures) pool->activeWait(future);
    
    //  Mark = limit between pre-calculations and path-wise operations
    //  Operations above mark have been propagated and accumulated
    //  We conduct one propagation mark to start
    //  On the main thread's tape
    Number::propagateMarkToStart();
    //  And on the worker thread's tapes
    Tape* mainThreadPtr = Number::tape;
    for (size_t i = 0; i < nThread; ++i)
    {
        if (mdlInit[i+1])
        {
            //  Set tape pointer
            Number::tape = &tapes[i];
            //  On that tape, propagate
            Number::propagateMarkToStart();
        }
    }
    //  Reset tape to main thread's
    Number::tape = mainThreadPtr;

    //  Sum sensitivities over threads
    for (size_t j = 0; j < nParam; ++j)
    {
        results.risks[j] = 0.0;
        for (size_t i = 0; i < models.size(); ++i)
        {
            if (mdlInit[i]) results.risks[j] += models[i]->parameters()[j]->adjoint();
        }
        results.risks[j] /= nPath;
    }

    //  Clear the main thread's tape
    //  The other tapes are cleared on the destruction of the vector of tapes
    Number::tape->clear();

    return results;
}