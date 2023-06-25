#ifndef TREE
#define TREE

#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include "globals.h"


struct Node;
struct Childitem;
struct BenchData;
struct SUID;


struct BenchData {
    BenchData(int numMeasurements) {
        results = new uint64_t[numMeasurements]();
        numAccumulated = 0;
    }

    BenchData(uint64_t* data, int numMeasurements) {
        results = new uint64_t[globals.defaultExp.numMeasurements]();
        for(int i = 0; i < numMeasurements; i++) {
            results[i] = data[i];
        }
        numAccumulated = 1;
    }

    BenchData() {
        results = new uint64_t[globals.defaultExp.numMeasurements]();
        numAccumulated = 0;
    }

    ~BenchData() {
        delete [] results;
    }

    int numAccumulated;
    uint64_t* results;
};

struct SUID {
    SUID(unsigned _regionNum, unsigned _SUNum) : regionNum(_regionNum), SUNum(_SUNum) {};
    unsigned regionNum;
    unsigned SUNum;

    bool operator==(const SUID& x) const{
        return (this->regionNum == x.regionNum && this->SUNum == x.SUNum);
    }

    struct HashFunction {
    size_t operator()(const SUID& x) const {
            return std::hash<uint64_t>()((((uint64_t)x.regionNum << 32)) + x.SUNum);
        }
    };
};


struct Node {
    Node(Node* _parent, SUID _edgeIn) : parent(_parent), done(false), regionRoot(false), edgeIn(_edgeIn) {
        nodeID = nextNodeID;
        nextNodeID++;
        if(parent) {
            this->depth = parent->depth + 1;
        } 
        else {
            this->depth = 0;
        }
    };
    // ~Node() {};

    Node* parent;
    bool done;
    bool regionRoot;
    SUID edgeIn;
    BenchData benchData;
    uint64_t nodeID;
    uint32_t depth;

    std::unordered_map<SUID, Node*, SUID::HashFunction> children;
    std::vector<uint64_t> doneChildren;
    std::unordered_set<SUID, SUID::HashFunction> completedEdges;

    static uint64_t nextNodeID;

};


void initNextRound();
void linkScheduler();
bool explorationFinished();
void accumulateBenchDataCurrentNode(uint64_t* data, int numExp, int numMeasurments);
void accumulateBenchDataCurrentNodeDefault(uint64_t* data);


#endif