#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <thread>
#include <boost/format.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>

#define ELPP_CUSTOM_COUT std::cerr
#define ELPP_THREAD_SAFE 1
#include "easylogging++.h"
#include "tclap/CmdLine.h"
#include "ProgramOpts.hpp"
#include "Alignment.hpp"
#include "AlnGraphBoost.hpp"
#include "DazAlnProvider.hpp"
#include "BoundedBuffer.hpp"

INITIALIZE_NULL_EASYLOGGINGPP

ProgramOpts popts;

typedef std::vector<dagcon::Alignment> AlnVec;

struct TargetData {
    std::string targSeq;
    AlnVec alns;
};

typedef BoundedBuffer<TargetData> TrgBuf;
typedef BoundedBuffer<std::string> CnsBuf;

void Reader(TrgBuf& trgBuf, AlnProvider* ap) {
    try {
        TargetData td;
        bool hasNext = true;
        do {
            hasNext = ap->nextTarget(td.targSeq, td.alns);
            //for (auto& aln : td.alns)
            //    std::cerr << aln;
            if (! td.alns.empty())
                trgBuf.push(td);
        } while (hasNext);
    }
    catch (PacBio::DagCon::IOException& e) {
        std::cerr << e.what();
        exit(1);
    }

    // write out sentinals, one per consensus thread
    TargetData sentinel;
    for (int i=0; i < popts.threads; i++)
        trgBuf.push(sentinel);
}

void Consensus(int id, TrgBuf& trgBuf, CnsBuf& cnsBuf) {
    int fake_well_counter; // just to avoid too many reads in the same bin
    TargetData td;
    trgBuf.pop(&td);
    std::vector<CnsResult> seqs;
    el::Loggers::getLogger("Consensus");

    while (td.alns.size() > 0) {
        if (td.alns.size() < popts.minCov) {
            trgBuf.pop(&td);
            continue;
        }
        boost::format msg("(%d) calling: %s Alignments: %d");
        CLOG(INFO, "Consensus") << msg % id % td.alns[0].id % td.alns.size();

        AlnGraphBoost ag(td.targSeq);
        AlnVec alns = td.alns;
        for (auto it = alns.begin(); it != alns.end(); ++it) {
            if (it->qstr.length() < popts.minLen) continue;
            dagcon::Alignment aln = normalizeGaps(*it);
            // XXX: Shouldn't be needed for dazcon, but causes some infinite
            // loops in the current consensus code.
            trimAln(aln, popts.trim);
            ag.addAln(aln);
        }
        CVLOG(3, "Consensus") << "Merging nodes";
        ag.mergeNodes();
        CVLOG(3, "Consensus") << "Generating consensus";
        ag.consensus(seqs, popts.minCov, popts.minLen);
        for (auto it = seqs.begin(); it != seqs.end(); ++it) {
            CnsResult result = *it;
            boost::format fasta(">%s/%d/%d_%d\n%s\n");
            fasta % alns[0].id
                  % fake_well_counter
                  % result.range[0]
                  % result.range[1]
                  % result.seq;
            cnsBuf.push(fasta.str());
            ++fake_well_counter;
        }
        trgBuf.pop(&td);
    }
    boost::format msg("(%d) ending ...");
    CLOG(INFO, "Consensus") << msg % id;
    // write out a sentinal
    cnsBuf.push("");
}

void Writer(CnsBuf& cnsBuf) {
    std::string cns;
    cnsBuf.pop(&cns);
    int sentinelCount = 0;
    while (true) {
        std::cout << cns;
        if (cns == "" && ++sentinelCount == popts.threads)
            break;

        cnsBuf.pop(&cns);
    }
}

void parseArgs(int argc, char **argv) {

    try {
        TCLAP::CmdLine cmd("PBI consensus module", ' ', "0.3");
        TCLAP::ValueArg<int> threadArg(
            "j","threads",                 // short, long name
            "Number of consensus threads", // description
             false, 4,                     // required, default
             "int", cmd);

        TCLAP::ValueArg<unsigned int> minCovArg(
            "c","min-coverage",
            "Minimum coverage for correction",
            false, 6, "uint", cmd);

        TCLAP::ValueArg<unsigned int> minLenArg(
            "l","min-len",
            "Minimum length for correction",
            false, 500, "uint", cmd);

        TCLAP::ValueArg<unsigned int> trimArg(
            "t","trim",
            "Trim alignments on either size",
            false, 10, "uint", cmd);

        TCLAP::ValueArg<std::string> alnFileArg(
            "a","align-file",
            "Path to the alignments file",
            true,"","string", cmd);

        TCLAP::ValueArg<std::string> seqFileArg(
            "s","seq-file",
            "Path to the sequences file",
            true,"","string", cmd);

        TCLAP::ValueArg<unsigned int> maxHitArg(
            "m","max-hit",
            "Maximum number of hits to pass to consensus",
            false,85,"uint", cmd);

        TCLAP::SwitchArg sortCovArg("x","coverage-sort",
            "Sort hits by coverage", cmd, false);

        TCLAP::SwitchArg properOvlArg("o","only-proper-overlaps",
            "Use only 'proper overlaps', i.e., align to the ends", cmd, false);

        TCLAP::SwitchArg verboseArg("v","verbose",
            "Turns on verbose logging", cmd, false);

        TCLAP::UnlabeledMultiArg<int> targetArgs(
            "targets", "Limit consensus to list of target ids",
            false, "list of ints", cmd);

        cmd.parse(argc, argv);

        popts.minCov     = minCovArg.getValue();
        popts.minLen     = minLenArg.getValue();
        popts.trim       = trimArg.getValue();
        popts.alnFile    = alnFileArg.getValue();
        popts.seqFile    = seqFileArg.getValue();
        popts.threads    = threadArg.getValue();
        popts.maxHits    = maxHitArg.getValue();
        popts.sortCov    = sortCovArg.getValue();
        popts.properOvls = properOvlArg.getValue();
        std::vector<int> tgs = targetArgs.getValue();
        popts.targets.insert(tgs.begin(), tgs.end());
    } catch (TCLAP::ArgException& e) {
        std::cerr << "Error " << e.argId() << ": " << e.error() << std::endl;
        exit(1);
    }
}

int main(int argc, char* argv[]) {
    parseArgs(argc, argv);
#if ELPP_ASYNC_LOGGING
    el::base::elStorage.reset(
       new el::base::Storage(el::LogBuilderPtr(new el::base::DefaultLogBuilder()),
                                               new el::base::AsyncDispatchWorker())
    );
#else
    el::base::elStorage.reset(
       new el::base::Storage(el::LogBuilderPtr(new el::base::DefaultLogBuilder()))
    );
#endif  // ELPP_ASYNC_LOGGING
    START_EASYLOGGINGPP(argc, argv);

    LOG(INFO) << "Initializing alignment provider";
    DazAlnProvider* ap;
    ap = new DazAlnProvider(popts);
    TrgBuf trgBuf(20);
    CnsBuf cnsBuf(10);

    std::thread writerThread(Writer, std::ref(cnsBuf));

    std::vector<std::thread> cnsThreads;
    for (int i=0; i < popts.threads; i++) {
        std::thread ct(Consensus, i, std::ref(trgBuf), std::ref(cnsBuf));
        cnsThreads.push_back(std::move(ct));
    }

    std::thread readerThread(Reader, std::ref(trgBuf), ap);

    writerThread.join();

    std::vector<std::thread>::iterator it;
    for (it = cnsThreads.begin(); it != cnsThreads.end(); ++it)
        it->join();

    readerThread.join();

    delete ap;

    return 0;
}
