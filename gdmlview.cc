/*
    gdmlview uses the Geant4 parser and openGL viewer to show GDML files
    Copyright (C) 2018  Wouter Deconinck

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "G4Types.hh"
#include "G4Version.hh"

#include "G4VUserDetectorConstruction.hh"
#include "G4VUserPhysicsList.hh"
#include "G4VUserPrimaryGeneratorAction.hh"

#include "G4GDMLParser.hh"
#include "G4PVPlacement.hh"
#include "G4RunManager.hh"
#include "G4UImanager.hh"
#include "G4UIQt.hh"
#include "G4VisExecutive.hh"
#include "G4UnitsTable.hh"

#include "G4ScaledSolid.hh"
#include "G4SubtractionSolid.hh"
#include "G4IntersectionSolid.hh"

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <string>
using std::string;
#include <algorithm>

#include <sys/param.h>

class DetectorConstruction: public G4VUserDetectorConstruction
{
  public:
    DetectorConstruction(const G4String& gdmlfile, bool validate, bool usecwd, bool overlap, int res, double tol, bool verbose, int errmax)
    : fValidate(validate),fUseCwd(usecwd),fOverlapCheck(overlap),
      fOverlapRes(res),fOverlapTol(tol),
      fVerbose(verbose),fErrMax(errmax)
    {
      new G4UnitDefinition("inch","in","Length",25.4*CLHEP::millimeter);
      SetGDMLFile(gdmlfile);
    };

    G4VPhysicalVolume* Construct() {
      G4cout << "Reading " << fFile << G4endl;
      G4cout << "- schema validation " << (fValidate? "on": "off") << G4endl;
      G4cout << "- overlap check " << (fOverlapCheck? "on": "off") << G4endl;
      // Change directory
      char cwd[MAXPATHLEN];
      if (!getcwd(cwd,MAXPATHLEN)) {
        G4cerr << __FILE__ << " line " << __LINE__ << ": ERROR no current working directory" << G4endl;
        exit(-1);
      }
      if (!fUseCwd && chdir(fPath)) {
        G4cerr << __FILE__ << " line " << __LINE__ << ": ERROR cannot change directory" << G4endl;
        exit(-1);
      }

      // Parse GDML file
      G4GDMLParser parser;
      parser.SetOverlapCheck(false); // do our own overlap check
      parser.Read((fUseCwd ? fPath + "/" + fFile : fFile), fValidate);
      G4VPhysicalVolume* worldvolume = parser.GetWorldVolume();
      AddTransparency(worldvolume,0.75);
      if (fOverlapCheck) {
        CheckOverlap(worldvolume, fOverlapRes, fOverlapTol, fVerbose, fErrMax);
        DrawOverlap();
      }
      // Change directory back
      if (!fUseCwd && chdir(cwd)) {
        G4cerr << __FILE__ << " line " << __LINE__ << ": ERROR cannot change directory" << G4endl;
        exit(-1);
      }
      // Turn world volume visible
      worldvolume->GetLogicalVolume()->SetVisAttributes(G4VisAttributes(true, G4Colour(1,1,1,0.1)));
      return worldvolume;
    };

    double AddTransparency(G4VPhysicalVolume* volume, G4double alpha) {
      G4double a = 1;
      auto n = volume->GetLogicalVolume()->GetNoDaughters();
      for (decltype(n) i = 0; i < n; i++)
        a = std::min(a, AddTransparency(volume->GetLogicalVolume()->GetDaughter(i), alpha));
      volume->GetLogicalVolume()->SetVisAttributes(G4VisAttributes(G4Colour(1,1,1,a)));
      return a * alpha;
    };

    void DrawOverlap() {
      for (std::vector< std::tuple<G4VPhysicalVolume*, G4VSolid* > >::const_iterator
            it  = fOverlaps.begin(); it != fOverlaps.end(); it++) {
        G4VPhysicalVolume* vol = std::get<0>(*it);
        G4VSolid* sol = std::get<1>(*it);
        G4LogicalVolume* log = new G4LogicalVolume(sol,0,"overlap_log",0,0,0);
        log->SetVisAttributes(G4VisAttributes(G4Colour::Yellow()));
        new G4PVPlacement(0,G4ThreeVector(),"overlap_phys",log,vol,false,0,false);
      }
    }

    void CheckOverlap(G4VPhysicalVolume* volume,
                      G4int res = 1000, G4double tol = 0.0,
                      G4bool verbose = true, G4int errMax = 1) {
      if (volume->CheckOverlaps(res, tol, verbose, errMax))
        volume->GetLogicalVolume()->SetVisAttributes(G4VisAttributes(G4Colour(1,0,0,0.5)));

      G4int trials = 0;
      G4VSolid* solid = volume->GetLogicalVolume()->GetSolid();
      G4LogicalVolume* motherLog = volume->GetMotherLogical();

      if (motherLog) {

        G4VSolid* motherSolid = motherLog->GetSolid();
        G4AffineTransform Tm(volume->GetRotation(), volume->GetTranslation());
        for (G4int ires = 0; ires < res; ires++) {
          G4ThreeVector point = solid->GetPointOnSurface();
          G4ThreeVector mp = Tm.TransformPoint(point);
          if (motherSolid->Inside(mp) == kOutside) {
            G4double distin = motherSolid->DistanceToIn(mp);
            if (distin > tol) {
              G4AffineTransform tf1(volume->GetRotation(),volume->GetTranslation());
              G4AffineTransform tf1i = tf1.Inverse();
              G4RotationMatrix* rot = new G4RotationMatrix(tf1i.NetRotation());
              G4ThreeVector trans = tf1i.NetTranslation();
              G4SubtractionSolid* overlap_solid = new G4SubtractionSolid("overlap_solid", solid, motherSolid, rot, trans);
              G4ScaledSolid* overlap_scaled_solid = new G4ScaledSolid("overlap_scaled_solid", overlap_solid, G4Scale3D(1.001, 1.001, 1.001));
              fOverlaps.push_back(std::make_tuple(volume,overlap_scaled_solid));
              if (verbose)
                G4cout << "Overlap of " << volume->GetName() << " with mother " << motherLog->GetName()
                       << " at " << point << " (" << distin/CLHEP::mm << " mm)" << G4endl;
              if (++trials > errMax) break;
            }
          }
          auto n = motherLog->GetNoDaughters();
          for (decltype(n) i = 0; i < n; i++) {
            G4VPhysicalVolume* daughter = motherLog->GetDaughter(i);
            if (daughter == volume) continue;
            G4AffineTransform Td( daughter->GetRotation(), daughter->GetTranslation() );
            G4ThreeVector md = Td.Inverse().TransformPoint(mp);
            G4VSolid* daughterSolid = daughter->GetLogicalVolume()->GetSolid();
            if (daughterSolid->Inside(md) == kInside) {
              G4double distout = daughterSolid->DistanceToOut(md);
              if (distout > tol) {
                G4AffineTransform tf1(volume->GetRotation(),volume->GetTranslation());
                G4AffineTransform tf2(daughter->GetRotation(),daughter->GetTranslation());
                G4AffineTransform tf21 = tf2 * tf1.Inverse();
                G4RotationMatrix* rot = new G4RotationMatrix(tf21.NetRotation());
                G4ThreeVector trans = tf21.NetTranslation();
                G4IntersectionSolid* overlap_solid = new G4IntersectionSolid("overlap_solid", solid, daughterSolid, rot, trans);
                G4ScaledSolid* overlap_scaled_solid = new G4ScaledSolid("overlap_scaled_solid", overlap_solid, G4Scale3D(1.001, 1.001, 1.001));
                fOverlaps.push_back(std::make_tuple(volume,overlap_scaled_solid));
                if (verbose)
                  G4cout << "Overlap of " << volume->GetName() << " with sister " << daughter->GetName()
                         << " at " << md << " (" << distout/CLHEP::mm << " mm)" << G4endl;
                if (++trials > errMax) break;
              }
            }
          }
        }
      }

      auto n = volume->GetLogicalVolume()->GetNoDaughters();
      for (decltype(n) i = 0; i < n; i++)
        CheckOverlap(volume->GetLogicalVolume()->GetDaughter(i), res, tol, verbose, errMax);
    }

  private:
    G4bool fValidate;
    G4bool fUseCwd;
    G4bool fOverlapCheck;
    G4int fOverlapRes;
    G4double fOverlapTol;
    G4bool fVerbose;
    G4int fErrMax;
    G4String fPath;
    G4String fFile;
    std::vector< std::tuple< G4VPhysicalVolume*, G4VSolid* > > fOverlaps;
    void SetGDMLFile(G4String gdmlfile) {
      size_t i = gdmlfile.rfind('/');
      if (i != std::string::npos) {
        fPath = gdmlfile.substr(0,i);
      } else fPath = ".";
      fFile = gdmlfile.substr(i + 1);
    }
};

class PhysicsList: public G4VUserPhysicsList
{
  protected:
    void ConstructParticle() { };
    void ConstructProcess() { };
};

class PrimaryGeneratorAction: public G4VUserPrimaryGeneratorAction
{
  public:
    virtual void GeneratePrimaries(G4Event*) { };
};

int main(int argc, char** argv)
{
  bool verbose = false;
  bool validate = false;
  bool usecwd = false;
  bool overlap = false;
  int res = 1000;
  int errmax = 1;
  double tol = 0.0;
  string gdmlfile;

  po::options_description desc("Allowed options");
  desc.add_options()
    ("help,h", "Produce this help message")
    ("verbose,v",  po::bool_switch(&verbose),    "enable verbose messages")
    ("gdmlfile,g", po::value<string>(&gdmlfile), "top level gdml file")
    ("schema,s",   po::bool_switch(&validate),   "enable schema validation")
    ("usecwd,c",   po::bool_switch(&usecwd),     "use paths relative to cwd")
    ("overlap,o",  po::bool_switch(&overlap),    "enable overlap check")
    ("tolerance,t",  po::value<double>(&tol),    "overlap tolerance in mm")
    ("resolution,r", po::value<int>(&res),       "overlap resolution as int")
    ("errmax,e",     po::value<int>(&errmax),    "overlap error max as int")
  ;
  po::positional_options_description p;
  p.add("gdmlfile", -1);

  // Parse command line options
  po::variables_map povm;
  po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), povm);
  po::notify(povm);

  if (povm.count("h") || gdmlfile.size() == 0) {
    G4cout << "    gdmlview  Copyright (C) 2018  Wouter Deconinck" << G4endl;
    G4cout << "This program comes with ABSOLUTELY NO WARRANTY; see LICENSE." << G4endl;
    G4cout << "This is free software, and you are welcome to redistribute it" << G4endl;
    G4cout << "under certain conditions; see LICENSE for details." << G4endl << G4endl;
    G4cout << desc << G4endl;
    return 1;
  }

  // Run manager
  G4RunManager* rm = new G4RunManager;
  rm->SetUserInitialization(new DetectorConstruction(gdmlfile,validate,usecwd,overlap,res,tol,verbose,errmax));
  rm->SetUserInitialization(new PhysicsList);
  rm->SetUserAction(new PrimaryGeneratorAction);
  rm->Initialize();

  // Visualization
  G4VisManager* vm = new G4VisExecutive("quiet");
  vm->Initialize();

  // Start user interface
  G4UIQt* ui = new G4UIQt(argc, argv);
  ui->GetUserInterfaceWidget()->setVisible(false);
  ui->GetCoutDockWidget()->setVisible(false);
  G4UImanager* um = G4UImanager::GetUIpointer();
  um->ApplyCommand("/vis/open OGLSQt 1200x800");
  um->ApplyCommand("/vis/drawVolume worlds");
  um->ApplyCommand("/vis/scene/add/axes 0 0 0");
  ui->AddMenu("views", "Views");
  ui->AddButton("views", "Front view (+Z)", "/vis/viewer/set/viewpointThetaPhi 180   0 deg");
  ui->AddButton("views", "Rear view (-Z)",  "/vis/viewer/set/viewpointThetaPhi   0   0 deg");
  ui->AddButton("views", "Right view (+X)", "/vis/viewer/set/viewpointThetaPhi +90 180 deg");
  ui->AddButton("views", "Left view (-X)",  "/vis/viewer/set/viewpointThetaPhi -90 180 deg");
  ui->AddButton("views", "Bottom view (+Y)","/vis/viewer/set/viewpointThetaPhi -90  90 deg");
  ui->AddButton("views", "Top view (-Y)",   "/vis/viewer/set/viewpointThetaPhi +90  90 deg");
  ui->AddMenu("options", "Options");
  ui->AddButton("options", "NbOfSides 24","/vis/viewer/set/lineSegmentsPerCircle 24");
  ui->AddButton("options", "NbOfSides 360","/vis/viewer/set/lineSegmentsPerCircle 360");
  ui->AddMenu("axes", "Axes");
  ui->AddButton("axes", "Add axes", "/vis/scene/add/axes 0 0 0");
  ui->AddButton("axes", "Hide axes", "/vis/scene/activateModel G4AxesModel false");
  ui->AddIcon("Front view (+Z)",  "user_icon", "/vis/viewer/set/viewpointThetaPhi 180   0 deg \n /vis/viewer/set/upVector 0 1 0", "TechDraw_ProjFront.xpm");
  ui->AddIcon("Rear view (-Z)",   "user_icon", "/vis/viewer/set/viewpointThetaPhi   0   0 deg \n /vis/viewer/set/upVector 0 1 0", "TechDraw_ProjRear.xpm");
  ui->AddIcon("Right view (+X)",  "user_icon", "/vis/viewer/set/viewpointThetaPhi +90 180 deg \n /vis/viewer/set/upVector 0 1 0", "TechDraw_ProjRight.xpm");
  ui->AddIcon("Left view (-X)",   "user_icon", "/vis/viewer/set/viewpointThetaPhi -90 180 deg \n /vis/viewer/set/upVector 0 1 0", "TechDraw_ProjLeft.xpm");
  ui->AddIcon("Bottom view (+Y)", "user_icon", "/vis/viewer/set/viewpointThetaPhi -90  90 deg \n /vis/viewer/set/upVector 1 0 0", "TechDraw_ProjBottom.xpm");
  ui->AddIcon("Top view (-Y)",    "user_icon", "/vis/viewer/set/viewpointThetaPhi +90  90 deg \n /vis/viewer/set/upVector 1 0 0", "TechDraw_ProjTop.xpm");
  ui->SessionStart();

  delete ui; delete vm; delete rm;
  return 0;
}
