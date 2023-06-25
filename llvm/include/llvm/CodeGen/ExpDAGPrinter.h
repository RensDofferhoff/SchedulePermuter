#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/SchedulePermuterHooks.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
using namespace llvm;

namespace llvm {
  template<>
  struct DOTGraphTraits<ScheduleDAG*> : public DefaultDOTGraphTraits {

  DOTGraphTraits (bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

    static std::string getGraphName(const ScheduleDAG *G) {
      return std::string(G->MF.getName());
    }

    static bool renderGraphFromBottomUp() {
      return true;
    }

    static bool isNodeHidden(const SUnit *Node, const ScheduleDAG *G) {
      return (Node->NumPreds > 10 || Node->NumSuccs > 10);
    }

    static std::string getNodeIdentifierLabel(const SUnit *Node,
                                              const ScheduleDAG *Graph) {
      std::string R;
      raw_string_ostream OS(R);
      OS << static_cast<const void *>(Node);
      return R;
    }

    /// If you want to override the dot attributes printed for a particular
    /// edge, override this method.
    static std::string getEdgeAttributes(const SUnit *Node,
                                         SUnitIterator EI,
                                         const ScheduleDAG *Graph) {
    if (EI.getSDep().getKind() == llvm::SDep::Anti)
      return "color=red,style=dashed";
    if (EI.getSDep().getKind() == llvm::SDep::Cluster)
      return "color=black,style=dashed";
    if (EI.getSDep().getKind() == llvm::SDep::Barrier)
      return "color=orange,style=dashed";
    if (EI.getSDep().getKind() == llvm::SDep::Artificial)
      return "color=purple,style=dashed";
    if (EI.getSDep().getKind() == SDep::Output)
      return "color=green,style=dashed";
    if (EI.isArtificialDep())
      return "color=cyan,style=dashed";
    if (EI.isCtrlDep())
      return "color=blue,style=dashed";
      return "";
    }


    std::string getNodeLabel(const SUnit *SU, const ScheduleDAG *Graph);
    static std::string getNodeAttributes(const SUnit *N,
                                         const ScheduleDAG *Graph) {
      return "shape=Mrecord";
    }

    static void addCustomGraphFeatures(ScheduleDAG *G,
                                       GraphWriter<ScheduleDAG*> &GW) {
      return G->addCustomGraphFeatures(GW);
    }
  };
}


void WriteExpGraph(ScheduleDAGInstrs* DAG, std::string name);