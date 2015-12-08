// -*- tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi: set et ts=4 sw=2 sts=2:

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include<iostream>
#include<dune/common/parallel/mpihelper.hh>

#if HAVE_DUNE_ALUGRID
#include<dune/alugrid/grid.hh>
#endif

#include<dune/grid/io/file/gmshreader.hh>
#include<dune/grid/io/file/vtk/subsamplingvtkwriter.hh>

#include<dune/pdelab/adaptivity/adaptivity.hh>
#include<dune/pdelab/backend/istl.hh>
#include<dune/pdelab/common/functionutilities.hh>
#include<dune/pdelab/finiteelementmap/pkfem.hh>
#include<dune/pdelab/gridfunctionspace/vtk.hh>

#include<dune/pdelab/gridfunctionspace/loadbalance.hh>

// Analytic function
template<typename GV, typename RF>
class U : public Dune::PDELab::AnalyticGridFunctionBase<
  Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1>,
  U<GV,RF> > {
public:
  using Traits = Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1>;
  using B = Dune::PDELab::AnalyticGridFunctionBase<Traits,U<GV,RF> >;

  U (const GV& gv) : B(gv) {}
  inline void evaluateGlobal (const typename Traits::DomainType& x,
                              typename Traits::RangeType& y) const
  {
    typename Traits::DomainType center(0.0);
    center -= x;
    y = exp(-1.0*center.two_norm2());
  }
};


int main(int argc, char** argv)
{
  try{
    // Maybe initialize MPI
    Dune::MPIHelper& helper = Dune::MPIHelper::instance(argc, argv);

#if HAVE_DUNE_ALUGRID
    // Create ALUGrid from gmsh file
    //
    // Note: The grid needs to be sufficient complex in order to find
    // load balancing problems. Simple structured grids are not suited
    // for this test.
    using Grid = Dune::ALUGrid<2,2,Dune::simplex,Dune::nonconforming>;
    Grid* grid;
    Dune::GridFactory<Grid> factory;
    if (helper.rank()==0){
      Dune::GmshReader<Grid>::read(factory, GRIDSDIR "/ldomain.msh", true, false);
    }
    grid = factory.createGrid();

    // Get leaf grid view
    using GV = Grid::LeafGridView;
    const GV gv = grid->leafGridView();

    // Do load balancing and print some information
    std::cout << "Load after reading /" << helper.rank() << "/ " << grid->size(0) << std::endl;
    grid->loadBalance();
    std::cout << "Load after load balance /" << helper.rank() << "/ " << grid->size(0) << std::endl;

    // Make analytic function
    using R = double;
    using AF = U<GV,R>;
    AF u(gv);

    // Integrate function
    typename AF::Traits::RangeType sum(0.0);
    integrateGridFunction(u, sum, 8);
    sum = gv.comm().sum(sum);
    if (gv.comm().rank()==0){
      std::cout << "Integrate analytical function: " << sum << std::endl;
    }

    // Create grid function space
    using D = typename GV::Grid::ctype;
    typedef Dune::PDELab::PkLocalFiniteElementMap<GV,D,R,2> FEM;
    FEM fem(gv);
    typedef Dune::PDELab::NoConstraints CON;
    typedef Dune::PDELab::ISTLVectorBackend<> VBE;
    typedef Dune::PDELab::GridFunctionSpace<GV,FEM,CON,VBE> GFS;
    GFS gfs(gv,fem);
    gfs.name("function");

    // Create coefficient vector and interpolate
    using X = Dune::PDELab::Backend::Vector<GFS,R>;
    X x(gfs,0.0);
    Dune::PDELab::interpolate(u,gfs,x);

    // Create discrete grid function and integrate
    using DGF = Dune::PDELab::DiscreteGridFunction<GFS,X>;
    DGF xdgf(gfs,x);
    integrateGridFunction(xdgf, sum, 8);
    sum = gfs.gridView().comm().sum(sum);
    if (gfs.gridView().comm().rank()==0){
      std::cout << "Integrate discrete grid function: " << sum << std::endl;
    }

    // Visualization
    Dune::SubsamplingVTKWriter<GV> vtkwriter(gv,0);
    Dune::PDELab::addSolutionToVTKWriter(vtkwriter,gfs,x);
    vtkwriter.addVertexData(std::make_shared<Dune::PDELab::VTKGridFunctionAdapter<AF> >(u,"u"));
    vtkwriter.write("before_refinement",Dune::VTK::ascii);

    // Mark cells on rank 0 for refinement
    if (gv.comm().rank()==0){
      std::cout << "Refine grid" << std::endl;
    }

    if (helper.rank()==0){
      for (const auto& cell : elements(grid->leafGridView())){
        grid->mark(1,cell);
      }
    }

    // Refine and adapt gfs and x
    adapt_grid(*grid,gfs,x,8);
    std::cout << "Load after refinement /" << helper.rank() << "/ " << grid->size(0) << std::endl;

    // Integrate discrete grid function after refinement
    DGF xdgfNew(gfs,x);
    integrateGridFunction(xdgfNew, sum, 8);
    sum = gfs.gridView().comm().sum(sum);
    if (gfs.gridView().comm().rank()==0){
      std::cout << "Integrate discrete grid function: " << sum << std::endl;
    }

    // Make load balancing and restore gfs and x
    if (gv.comm().rank()==0){
      std::cout << "Rebalance load" << std::endl;
    }
    loadBalanceGrid(*grid,gfs,x);
    std::cout << "load after load balance /" << helper.rank() << "/ " << grid->size(0) << std::endl;

    // Integrate discrete grid function after load balancing
    DGF xdgfNewNew(gfs,x);
    typename AF::Traits::RangeType sum2(0.0);
    integrateGridFunction(xdgfNewNew, sum2, 8);
    sum2 = gfs.gridView().comm().sum(sum2);
    if (gfs.gridView().comm().rank()==0){
      std::cout << "Integrate discrete grid function: " << sum2 << std::endl;
    }

    // Visualization
    AF uNew(gfs.gridView());
    Dune::SubsamplingVTKWriter<GV> vtkwriterNew(gfs.gridView(),0);
    vtkwriterNew.addVertexData(std::make_shared<Dune::PDELab::VTKGridFunctionAdapter<AF> >(uNew,"u"));
    Dune::PDELab::addSolutionToVTKWriter(vtkwriterNew,gfs,x);
    vtkwriterNew.write("after_load_balance",Dune::VTK::ascii);

    // Print difference between integrals
    if (gfs.gridView().comm().rank()==0){
      std::cout << "Difference between numerical integrals: " << std::abs(sum-sum2) << std::endl;
    }

    // If difference is too large the test fails
    if (std::abs(sum-sum2)>1e-13){
      return 1;
    }

#endif

    return 0;
  }
  catch (Dune::Exception &e){
    std::cerr << "Dune reported error: " << e << std::endl;
  }
  catch (...){
    std::cerr << "Unknown exception thrown!" << std::endl;
  }
}
