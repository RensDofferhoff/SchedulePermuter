#include "tree.h"
#include "globals.h"
#include <unordered_map>
#include <unordered_set>
#include <random>
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/CodeGen/SchedulePermuterHooks.h"

using namespace llvm;

static long permutations;
static bool newRegion;
static Node* currentNode;
static Node* root;
static unsigned regionNum;
static std::unordered_map<SUID, SUnit*, SUID::HashFunction> SUnitMap;
static std::unordered_map<SUID, std::string, SUID::HashFunction> edgeNameMap;

static std::random_device rd;
static std::mt19937 gen(rd()); 
bool (*selection)(std::pair<SUID, Node*>& selected);

static llvm::SUnit* pickChild(bool &IsTopNode);
static void addChild(Node* node, SUID newEdge);
static void nextRegion();

uint64_t Node::nextNodeID = 0;


static bool selectRandom(std::pair<SUID, Node*>& selected) {
    if(currentNode->children.size() == 0)
        return false;
    
    std::uniform_int_distribution<> distr(0, currentNode->children.size() - 1); 
    auto it = currentNode->children.begin();
    std::advance(it, distr(gen));
    selected = *it;
    return true;

}

static bool selectFirst(std::pair<SUID, Node*>& selected) {
    if(currentNode->children.size() != 0) {
        selected = *currentNode->children.begin();
        return true;
    }
    return false;
}


SUnit* pickChild(bool &IsTopNode) {
    // outs() << "pickChild" << currentNode->nodeID << "\n" ;
    if(!currentNode->regionRoot || newRegion) {
        std::pair<SUID, Node*> selected = {{0,0}, nullptr};
        bool found = selection(selected);
        auto unitID = selected.first;
        auto child = selected.second;

        if(found) {  
            // outs() << unitID.SUNum << "picked\n" << child <<" #numchild " << child->children.size() << "\n";
            auto res = SUnitMap.find(unitID);
            assert(res != SUnitMap.end() && "node was not added?");
            // outs() << "chosen:" << child->nodeID << "\n";

            //add all choices that carry over to child
            for(auto& opt : currentNode->children) {
                // outs() << opt.second->nodeID << " ";
                addChild(child, opt.first);
            }
            for(auto& id : currentNode->completedEdges) {
               addChild(child, id);
            }
            // outs() << "\n";
            //deleted child from self
            delete child->children[unitID];
            child->children.erase(unitID);
            // outs() << unitID.regionNum << "picked\n" << child <<" #numchild " << child->children.size() << "\n";
            currentNode = child;
            IsTopNode = true;
            newRegion = false;
            return res->second;   
        }  

    }
    // outs() << "empty\n" ;
    newRegion = false;
    return nullptr; //no more instructions to schedule
}

static void registerEdgeName(SUID id, SUnit* unit) {
    std::string name = "";
    llvm::raw_string_ostream ss(name);
    unit->getInstr()->print(ss);
    edgeNameMap.insert({id, name});
}

static void addChild(Node* node, SUID newEdge) {
    if(node->children.find(newEdge) == node->children.end() && 
    node->completedEdges.find(newEdge) == node->completedEdges.end()) {
        Node* child = new Node(node, newEdge);
        node->children.insert({newEdge, child});
    }
}

void addSchedChild(SUnit *SU) {
    SUID id = {regionNum, SU->NodeNum};
    SUnitMap.insert({id, SU});
    registerEdgeName(id, SU);
    addChild(currentNode, id);
}


void nextRegion() {
    currentNode->regionRoot = true;
    newRegion = true;
    regionNum++;
}


static void writeNodeToDisk(Node* node) {
    globals.treeOut << node->nodeID;
    globals.treeOut << node->depth;
    if(node->parent)
        globals.treeOut << node->parent->nodeID;
    else 
        globals.treeOut << (uint64_t)0;
    globals.treeOut << edgeNameMap[node->edgeIn];
    for(int i = 0; i < globals.defaultExp.numMeasurements; i++) {
        globals.treeOut << (uint64_t) node->benchData.results[i];
    }
    globals.treeOut << parquet::EndRow;
}

static void normalizeBenchData(Node* node, int numMeasurments) {
    for(int i = 0; i < numMeasurments; i++) {
        node->benchData.results[i] /= node->benchData.numAccumulated;
    }
}

static void accumulateBenchData(Node* node, BenchData* data, int numMeasurments) {
    node->benchData.numAccumulated++;
    for(int i = 0; i < numMeasurments; i++) {
        node->benchData.results[i] += data->results[i];
    }
}


static void accumulateBenchDataParent(Node* node, int numMeasurments) {
    if(node->parent) {
        accumulateBenchData(node->parent, &node->benchData, numMeasurments);
    }
}

void accumulateBenchDataCurrentNode(uint64_t* data, int numExp, int numMeasurments) {
    for(int i = 0; i < numExp; i++) {
        BenchData benchData(data + i * numMeasurments, numMeasurments);
        accumulateBenchData(currentNode, &benchData, numMeasurments);
    }
}


void accumulateBenchDataCurrentNodeDefault(uint64_t* data) {
    accumulateBenchDataCurrentNode(data, globals.defaultExp.numExpPerSchedule, globals.defaultExp.numMeasurements);
}


void initNextRound() {
    // outs() << "nextRound\n";
    assert(currentNode->doneChildren.size() == 0 && "Did not end in a region leaf node?!");
    regionNum = 0;

    //if check if all children have been visited normalize, accumulate data and write to disk delete after.
    //move up to parent
    while (currentNode) {
        if(verbose)
            outs() << "r: " << currentNode->children.size() << " d: " << currentNode->doneChildren.size() << "\n";
        Node* parent = currentNode->parent;
        if(currentNode->children.size() == 0) {
            if(parent) {
                parent->children.erase(currentNode->edgeIn);
                parent->doneChildren.push_back(currentNode->nodeID);
                parent->completedEdges.insert(currentNode->edgeIn);
            }
            normalizeBenchData(currentNode, globals.defaultExp.numMeasurements);
            accumulateBenchDataParent(currentNode, globals.defaultExp.numMeasurements);
            writeNodeToDisk(currentNode);
            if(currentNode != root)
                delete currentNode;
        }
        currentNode = parent;
    }

    SUnitMap.clear();
    edgeNameMap.clear();
    permutations++;
    currentNode = root;
}


void linkScheduler() {
    initExtern = &nextRegion;
    releaseTopNodeExtern = &addSchedChild;
    pickNodeExtern = &pickChild;
    root = currentNode = new Node(nullptr, {0,0});
    permutations = 0;
    regionNum = 0;

    if(globals.defaultExp.selectionMode == SelectionMode::select_first) {
        selection = &selectFirst;
    }
    else {
        selection = &selectRandom;
    }
}

bool explorationFinished() {
    if(!root)
        return false;

    if(root->children.size() == 0) {
        delete root;
        root = nullptr;
        return true;
    }
    
    return false;
}
