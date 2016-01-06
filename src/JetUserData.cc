#include "FWCore/Framework/interface/EDProducer.h"
#include "FWCore/Utilities/interface/InputTag.h"
#include "DataFormats/Common/interface/Handle.h"
#include "FWCore/Framework/interface/ESHandle.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/Utilities/interface/EDMException.h"

#include "DataFormats/PatCandidates/interface/Jet.h"
#include "DataFormats/PatCandidates/interface/Electron.h"
#include "DataFormats/PatCandidates/interface/Muon.h"

// Fastjet (for creating subjets)
#include <fastjet/JetDefinition.hh>
#include <fastjet/PseudoJet.hh>
#include "fastjet/tools/Filter.hh"
#include <fastjet/ClusterSequence.hh>
#include <fastjet/ClusterSequenceArea.hh>

// Vertex
#include "DataFormats/VertexReco/interface/Vertex.h"
#include "DataFormats/VertexReco/interface/VertexFwd.h"

// trigger
#include "HLTrigger/HLTcore/interface/HLTConfigProvider.h"
#include "DataFormats/Common/interface/TriggerResults.h"
#include "DataFormats/HLTReco/interface/TriggerEvent.h"
#include "DataFormats/HLTReco/interface/TriggerObject.h"
#include "DataFormats/HLTReco/interface/TriggerTypeDefs.h" // gives access to the (release cycle dependent) trigger object codes
#include "DataFormats/JetReco/interface/Jet.h"

// JEC
#include "CondFormats/JetMETObjects/interface/JetCorrectorParameters.h"
#include "CondFormats/JetMETObjects/interface/JetCorrectionUncertainty.h"
#include "JetMETCorrections/Objects/interface/JetCorrectionsRecord.h"

#include <TFile.h>
#include <TH1F.h>
#include <TGraphAsymmErrors.h>
#include <TLorentzVector.h>
#include <vector>

using namespace fastjet;
using namespace reco;
using namespace edm;
using namespace std;
using namespace trigger;

typedef std::vector<pat::Jet> PatJetCollection;

class JetUserData : public edm::EDProducer {
  public:
    JetUserData( const edm::ParameterSet & );   

  private:
    void produce( edm::Event &, const edm::EventSetup & );
    bool isMatchedWithTrigger(const pat::Jet&, trigger::TriggerObjectCollection,int&,double&,double);
    double getResolutionRatio(double eta);
    double getJERup(double eta);
    double getJERdown(double eta);

    edm::EDGetTokenT<std::vector<pat::Jet> >     jetToken_;

    //InputTag jetLabel_;
    InputTag jLabel_; 
    InputTag triggerResultsLabel_, triggerSummaryLabel_;
    InputTag hltJetFilterLabel_;
    std::string hltPath_;
    double hlt2reco_deltaRmax_;
    std::string jecCorrection_;
    HLTConfigProvider hltConfig;
    int triggerBit;
};


JetUserData::JetUserData(const edm::ParameterSet& iConfig) :
  jLabel_             (iConfig.getParameter<edm::InputTag>("jetLabel")),
  triggerResultsLabel_(iConfig.getParameter<edm::InputTag>("triggerResults")),
  triggerSummaryLabel_(iConfig.getParameter<edm::InputTag>("triggerSummary")),
  hltJetFilterLabel_  (iConfig.getParameter<edm::InputTag>("hltJetFilter")),   //trigger objects we want to match
  hltPath_            (iConfig.getParameter<std::string>("hltPath")),
  hlt2reco_deltaRmax_ (iConfig.getParameter<double>("hlt2reco_deltaRmax")),
  jecCorrection_       (iConfig.getParameter<std::string>("jecCorrection"))
{
  produces<vector<pat::Jet> >();
}


void JetUserData::produce( edm::Event& iEvent, const edm::EventSetup& iSetup) {

  bool isMC = (!iEvent.isRealData());

  edm::Handle<std::vector<pat::Jet> > jetHandle, packedjetHandle;
  iEvent.getByLabel(jLabel_, jetHandle);
  auto_ptr<vector<pat::Jet> > jetColl( new vector<pat::Jet> (*jetHandle) );


  //// TRIGGER (this is not really needed ...)
  bool changedConfig = false;
  bool pathFound = false;
  if (!hltConfig.init(iEvent.getRun(), iSetup, "HLT", changedConfig)) {
    edm::LogError("HLTConfigProvider") << "Initialization of HLTConfigProvider failed!!" << std::endl;
    return;
  }

  if (changedConfig){
    edm::LogInfo("HLTMenu") << "the current menu is " << hltConfig.tableName() << std::endl;
    triggerBit = -1;
    for (size_t j = 0; j < hltConfig.triggerNames().size(); j++) {
      if (TString(hltConfig.triggerNames()[j]).Contains(hltPath_)) {triggerBit = j;pathFound=true;}
    }
    if (triggerBit == -1) edm::LogError("NoHLTPath") << "HLT path not found" << std::endl;
  }

     edm::Handle<edm::TriggerResults> triggerResults;
     iEvent.getByLabel(triggerResultsLabel_, triggerResults);
  /* Why do we need this
     if (size_t(triggerBit) < triggerResults->size() && pathFound)
       if (triggerResults->accept(triggerBit))
         std::cout << "event pass : " << hltPath_ << std::endl;
  */ 

  //// TRIGGER MATCHING
  trigger::TriggerObjectCollection JetLegObjects;

  edm::Handle<trigger::TriggerEvent> triggerSummary;

  if ( triggerSummary.isValid() ) {
    iEvent.getByLabel(triggerSummaryLabel_, triggerSummary);

    // Results from TriggerEvent product - Attention: must look only for
    // modules actually run in this path for this event!
    if(pathFound){
      const unsigned int triggerIndex(hltConfig.triggerIndex(hltPath_));
      const vector<string>& moduleLabels(hltConfig.moduleLabels(triggerIndex));
      const unsigned int moduleIndex(triggerResults->index(triggerIndex));
      for (unsigned int j=0; j<=moduleIndex; ++j) {
        const string& moduleLabel(moduleLabels[j]);
        const string  moduleType(hltConfig.moduleType(moduleLabel));
        // check whether the module is packed up in TriggerEvent product
        const unsigned int filterIndex(triggerSummary->filterIndex(InputTag(moduleLabel,"","HLT")));
        if (filterIndex<triggerSummary->sizeFilters()) {
          TString lable = moduleLabel.c_str();
          if (lable.Contains(hltJetFilterLabel_.label())) {

            const trigger::Vids& VIDS (triggerSummary->filterIds(filterIndex));
            const trigger::Keys& KEYS(triggerSummary->filterKeys(filterIndex));
            const size_type nI(VIDS.size());
            const size_type nK(KEYS.size());
            assert(nI==nK);
            const size_type n(max(nI,nK));
            const trigger::TriggerObjectCollection& TOC(triggerSummary->getObjects());
            for (size_type i=0; i!=n; ++i) {
              const trigger::TriggerObject& TO(TOC[KEYS[i]]);
              JetLegObjects.push_back(TO);	  
            }
          }
        }
      }
    }
  }

  // JEC Uncertainty
  edm::ESHandle<JetCorrectorParametersCollection> JetCorrParColl;
  iSetup.get<JetCorrectionsRecord>().get(jecCorrection_, JetCorrParColl); 
  JetCorrectorParameters const & JetCorrPar = (*JetCorrParColl)["Uncertainty"];
  JetCorrectionUncertainty *jecUnc = new JetCorrectionUncertainty(JetCorrPar);

  for (size_t i = 0; i< jetColl->size(); i++){
    pat::Jet & jet = (*jetColl)[i];

    //Individual jet operations to be added here
    // trigger matched 
    int idx       = -1;
    double deltaR = -1.;
    bool isMatched2trigger = isMatchedWithTrigger(jet, JetLegObjects, idx, deltaR, hlt2reco_deltaRmax_) ;
    double hltEta = ( isMatched2trigger ? JetLegObjects[0].eta()    : -999.);
    double hltPhi = ( isMatched2trigger ? JetLegObjects[0].phi()    : -999.);
    double hltPt  = ( isMatched2trigger ? JetLegObjects[0].pt()     : -999.);
    double hltE   = ( isMatched2trigger ? JetLegObjects[0].energy() : -999.);

    // SMEARING
    // http://twiki.cern.ch/twiki/bin/view/CMS/JetResolution
    reco::Candidate::LorentzVector smearedP4;
    if(isMC) {
      const reco::GenJet* genJet=jet.genJet();
      if(genJet) {
        float smearFactor=getResolutionRatio(jet.eta());
        smearedP4=jet.p4()-genJet->p4();
        smearedP4*=smearFactor; // +- 3*smearFactorErr;
        smearedP4+=genJet->p4();
      }
    } else {
      smearedP4=jet.p4();
    }
    // JER
    double JER     = getResolutionRatio(jet.eta());
    double JERup   = getJERup  (jet.eta());
    double JERdown = getJERdown(jet.eta());

    jet.addUserFloat("HLTjetEta",   hltEta);
    jet.addUserFloat("HLTjetPhi",   hltPhi);
    jet.addUserFloat("HLTjetPt",    hltPt);
    jet.addUserFloat("HLTjetE",     hltE);
    jet.addUserFloat("HLTjetDeltaR",deltaR);

    jet.addUserFloat("SmearedPEta", smearedP4.eta());
    jet.addUserFloat("SmearedPhi",  smearedP4.phi());
    jet.addUserFloat("SmearedPt",   smearedP4.pt());
    jet.addUserFloat("SmearedE",    smearedP4.energy());

    jet.addUserFloat("JER",     JER);
    jet.addUserFloat("JERup",   JERup);
    jet.addUserFloat("JERdown", JERdown);

    // JEC uncertainty
    jecUnc->setJetEta(jet.eta());
    jecUnc->setJetPt (jet.pt());
    double jecUncertainty = jecUnc->getUncertainty(true);
    jet.addUserFloat("jecUncertainty",   jecUncertainty);

    TLorentzVector jetp4 ; 
    jetp4.SetPtEtaPhiE(jet.pt(), jet.eta(), jet.phi(), jet.energy()) ; 

    //// Jet constituent indices for lepton matching
    std::vector<unsigned int> constituentIndices;
    auto constituents = jet.daughterPtrVector();
    for ( auto & constituent : constituents ) {
      constituentIndices.push_back( constituent.key() );
    }

    jet.addUserData("pfKeys", constituentIndices );


  } //// Loop over all jets 

  iEvent.put( jetColl );

  delete jecUnc;
}

// ------------ method called once each job just after ending the event loop  ------------
  bool
JetUserData::isMatchedWithTrigger(const pat::Jet& p, trigger::TriggerObjectCollection triggerObjects, int& index, double& deltaR, double deltaRmax = 0.2)
{
  for (size_t i = 0 ; i < triggerObjects.size() ; i++){
    float dR = sqrt(pow(triggerObjects[i].eta()-p.eta(),2)+ pow(acos(cos(triggerObjects[i].phi()-p.phi())),2)) ;
    if (dR<deltaRmax) {
      deltaR = dR;
      index  = i;
      return true;
    }
  }
  return false;
}

// JER Updated to:
// https://twiki.cern.ch/twiki/bin/view/CMS/JetResolution?rev=41#JER_Scaling_factors_and_Uncertai
double
JetUserData::getResolutionRatio(double eta)
{
  eta=fabs(eta);
  if(eta>=0.0 && eta<0.8) return 1.061; // +-0.023
  if(eta>=0.8 && eta<1.3) return 1.088; // +-0.029
  if(eta>=1.3 && eta<1.9) return 1.106; // +-0.030   
  if(eta>=1.9 && eta<2.5) return 1.126; // +-0.094 
  if(eta>=2.5 && eta<3.0) return 1.343; // +-0.123 
  if(eta>=3.0 && eta<3.2) return 1.303; // +-0.111 
  if(eta>=3.2 && eta<5.0) return 1.320; // +-0.286 
  return -1.;
}

double
JetUserData::getJERup(double eta)
{
  eta=fabs(eta);
  if(eta>=0.0 && eta<0.8) return 1.084;
  if(eta>=0.8 && eta<1.3) return 1.117;
  if(eta>=1.3 && eta<1.9) return 1.136;
  if(eta>=1.9 && eta<2.5) return 1.220;
  if(eta>=2.5 && eta<3.0) return 1.466;
  if(eta>=3.0 && eta<3.2) return 1.414;
  if(eta>=3.2 && eta<5.0) return 1.606;
  return -1.;  
}

double
JetUserData::getJERdown(double eta)
{
  eta=fabs(eta);
  if(eta>=0.0 && eta<0.8) return 1.038;
  if(eta>=0.8 && eta<1.3) return 1.059;
  if(eta>=1.3 && eta<1.9) return 1.076;
  if(eta>=1.9 && eta<2.5) return 1.032;
  if(eta>=2.5 && eta<3.0) return 1.220;
  if(eta>=3.0 && eta<3.2) return 1.192;
  if(eta>=3.2 && eta<5.0) return 1.034;
  return -1.;  
}


#include "FWCore/Framework/interface/MakerMacros.h"


DEFINE_FWK_MODULE(JetUserData);
