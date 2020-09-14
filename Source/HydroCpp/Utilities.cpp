#include <Utilities.H>

/*
AMREX_GPU_DEVICE
void
pc_cmpTemp(
           const int i, const int j, const int k, const int FirstSpec, const int NumSpec,
           amrex::Array4<amrex::Real> const& S)
{
  amrex::Real rhoInv = 1.0 / S(i, j, k, URHO);
  amrex::Real T = S(i, j, k, UTEMP);
  amrex::Real e = S(i, j, k, UEINT) * rhoInv;
  amrex::Real massfrac[NumSpec];
  for (int n = 0; n < NumSpec; ++n) {
    massfrac[n] = S(i, j, k, FirstSpec + n) * rhoInv;
  }
  EOS::EY2T(e, massfrac, T);
  S(i, j, k, UTEMP) = T;
}*/

AMREX_GPU_DEVICE
void
pc_rst_int_e(
  const int i, const int j, const int k, amrex::Array4<amrex::Real> const& S)
{
  amrex::Real rho = S(i, j, k, URHO);
  amrex::Real u = S(i, j, k, UMX) / rho;
  amrex::Real v = S(i, j, k, UMY) / rho;
  amrex::Real w = S(i, j, k, UMZ) / rho;
  amrex::Real ke = 0.5 * (u * u + v * v + w * w);
  S(i, j, k, UEINT) = S(i, j, k, UEDEN) - rho * ke;
}

// -----------------------------------------------------------
// Read a binary file
// INPUTS/OUTPUTS:
// iname => filename
// nx    => input resolution
// ny    => input resolution
// nz    => input resolution
// data  <= output data
// -----------------------------------------------------------
void
read_binary(
  const std::string iname,
  const size_t nx,
  const size_t ny,
  const size_t nz,
  const size_t ncol,
  amrex::Vector<double>& data /*needs to be double*/)
{

  std::ifstream infile(iname, std::ios::in | std::ios::binary);
  if (not infile.is_open()) {
    amrex::Abort("Unable to open input file " + iname);
  }

  for (int i = 0; i < nx * ny * nz * ncol; i++) {
    infile.read(reinterpret_cast<char*>(&data[i]), sizeof(data[i]));
  }
  infile.close();
} 

// -----------------------------------------------------------
// Read a csv file
// INPUTS/OUTPUTS:
// iname => filename
// nx    => input resolution
// ny    => input resolution
// nz    => input resolution
// data  <= output data
// -----------------------------------------------------------
void
read_csv(
  const std::string iname,
  const size_t nx,
  const size_t ny,
  const size_t nz,
  amrex::Vector<amrex::Real>& data)
{
  std::ifstream infile(iname, std::ios::in);
  const std::string memfile = read_file(infile);
  if (not infile.is_open()) {
    amrex::Abort("Unable to open input file " + iname);
  }
  infile.close();
  std::istringstream iss(memfile);

  // Read the file
  int nlines = 0;
  std::string firstline, line;
  std::getline(iss, firstline); // skip header
  while (getline(iss, line))
    ++nlines;

  // Quick sanity check
  if (nlines != nx * ny * nz)
    amrex::Abort(
      "Number of lines in the input file (= " + std::to_string(nlines) +
      ") does not match the input resolution (=" + std::to_string(nx) + ")");

  // Read the data from the file
  iss.clear();
  iss.seekg(0, std::ios::beg);
  std::getline(iss, firstline); // skip header
  int cnt = 0;
  while (std::getline(iss, line)) {
    std::istringstream linestream(line);
    std::string value;
    while (getline(linestream, value, ',')) {
      std::istringstream sinput(value);
      sinput >> data[cnt];
      cnt++;
    }
  }
}

// -----------------------------------------------------------
// Search for the closest index in an array to a given value
// using the bisection technique.
// INPUTS/OUTPUTS:
// xtable(0:n-1) => array to search in (ascending order)
// n             => array size
// x             => x location
// idxlo        <=> output st. xtable(idxlo) <= x < xtable(idxlo+1)
// -----------------------------------------------------------
AMREX_GPU_HOST_DEVICE
void
locate(const amrex::Real* xtable, const int n, amrex::Real& x, int& idxlo)
{
  // If x is out of bounds, return boundary index
  if (x >= xtable[n - 1]) {
    idxlo = n - 1;
    return;
  } else if (x <= xtable[0]) {
    idxlo = 0;
    return;
  }

  // Do the bisection
  idxlo = 0;
  int idxhi = n - 1;
  bool notdone = true;
  while (notdone) {
    if (idxhi - idxlo <= 1) {
      notdone = false;
    } else {
      const int idxmid = (idxhi + idxlo) / 2;
      if (x >= xtable[idxmid]) {
        idxlo = idxmid;
      } else {
        idxhi = idxmid;
      }
    }
  }
}

using namespace amrex;

AMREX_GPU_DEVICE
void limit_hydro_fluxes_on_small_dens(const Box& bx,
                                      int idir,
                                      Array4<Real const> const& u,
                                      Array4<Real const> const& q,
                                      Array4<Real> const& flux,
                                      Real small_dens, Real lcfl,
                                      Real dx_dir, Real dt)
{

    // The following algorithm comes from Hu, Adams, and Shu (2013), JCP, 242, 169,
    // "Positivity-preserving method for high-order conservative schemes solving
    // compressible Euler equations." It has been modified to enforce not only positivity
    // but also the stronger requirement that rho > small_dens. We do not limit on pressure
    // (or, similarly, internal energy) because those cases are easily fixed by calls to
    // reset_internal_energy that enforce a thermodynamic floor. The density limiter, by
    // contrast, is very important because calls to enforce_minimum_density can yield
    // hydrodynamic states that are inconsistent (there is no clear strategy for what to do
    // when a density is negative).

    const Real density_floor_tolerance = 1.1;

    // The density floor is the small density, modified by a small factor.
    // In practice numerical error can cause the density that is created
    // by this flux limiter to be slightly lower than the target density,
    // so we set the target to be slightly larger than the real density floor
    // to avoid density resets.

    Real density_floor = small_dens * density_floor_tolerance;

    // We apply this flux limiter on a per-edge basis. So we can guarantee
    // that any individual flux cannot cause a small density in one step,
    // but with the above floor we cannot guarantee that the sum of the
    // fluxes will enforce this constraint. The only way to guarantee that
    // is if the density floor is increased by a factor of the number of
    // edges, so that even if all edges are summed together, the density
    // will still be at the floor. So we multiply the floor by a factor of
    // 2 (two edges in each dimension) and a factor of AMREX_SPACEDIM.

    density_floor *= AMREX_SPACEDIM * 2;

    Real dtdx = dt / dx_dir;
    Real alpha = 1.0 / AMREX_SPACEDIM;

    amrex::ParallelFor(bx,
    [=] AMREX_GPU_HOST_DEVICE (int i, int j, int k)
    {
        // Grab the states on either side of the interface we are working with,
        // depending on which dimension we're currently calling this with.

        GpuArray<Real, QVAR> uR;
        GpuArray<Real, QVAR> uL;
        GpuArray<Real, QVAR> qR;
        GpuArray<Real, QVAR> qL;
        GpuArray<int, 3> idxR = {i,j,k};
        GpuArray<int, 3> idxL;

        if (idir == 0) {
            idxL = {i-1,j,k};
        } else if (idir == 1) {
            idxL = {i,j-1,k};
        } else {
            idxL = {i,j,k-1};
        }

        for (int n = 0; n < QVAR; ++n) {
            uR[n] = u(i,j,k,n);
            qR[n] = q(i,j,k,n);
            uL[n] = u(idxl[0],idxl[1],idxL[2]);
            qL[n] = q(idxl[0],idxl[1],idxL[2]);
        }

        // If an adjacent zone has a floor-violating density, set the flux to zero and move on.
        // At that point, the only thing to do is wait for a reset at a later point.

        if (uR[URHO] < density_floor || uL[URHO] < density_floor) {

            for (int n = 0; n < NGDNV; ++n) {
                flux(i,j,k,n) = 0.0;
            }

            return;
        }

        // Construct cell-centered fluxes.
        GpuArray<Real, NGDNV> fluxL;
        GpuArray<Real, NGDNV> fluxR;

        for (int n = 0; n < NGDNV; ++n) {
            fluxL[n] = 0.0;
            fluxR[n] = 0.0;
        }

        fluxL[URHO]      = uL[URHO] * qL[QU + idir];
        fluxL[UMX]       = uL[UMX]  * qL[QU + idir];
        fluxL[UMY]       = uL[UMY]  * qL[QU + idir];
        fluxL[UMZ]       = uL[UMZ]  * qL[QU + idir];
        fluxL[UEDEN]     = (uL[UEDEN] + qL[QPRES]) * qL[QU + idir];
        fluxL[UEINT]     = uL[UEINT]  * qL[QU + idir];

        fluxR[URHO]      = uR[URHO] * qR[QU + idir];
        fluxR[UMX]       = uR[UMX]  * qR[QU + idir];
        fluxR[UMY]       = uR[UMY]  * qR[QU + idir];
        fluxR[UMZ]       = uR[UMZ]  * qR[QU + idir];
        fluxR[UEDEN]     = (uR[UEDEN] + qR[QPRES]) * qR[QU + idir];
        fluxR[UEINT]     = uR[UEINT] * qR[QU + idir];

        fluxL[UMX + idir] = fluxL[UMX + idir] + qL[QPRES];
        fluxR[UMX + idir] = fluxR[UMX + idir] + qR[QPRES];

        // Construct the Lax-Friedrichs flux on the interface (Equation 12).
        // Note that we are using the information from Equation 9 to obtain the
        // effective maximum wave speed, (|u| + c)_max = CFL / lambda where
        // lambda = dt/(dx * alpha); alpha = 1 in 1D and may be chosen somewhat
        // freely in multi-D as long as alpha_x + alpha_y + alpha_z = 1.

        GpuArray<Real, NGDNV> fluxLF;
        for (int n = 0; n < NGDNV; ++n) {
            fluxLF[n] = 0.5 * (fluxL[n] + fluxR[n] + (lcfl / dtdx / alpha) * (uL[n] - uR[n]));
        }

        // Coefficients of fluxes on either side of the interface.

        Real flux_coefR = 2.0 * (dtdx / alpha);
        Real flux_coefL = 2.0 * (dtdx / alpha);

        // Obtain the one-sided update to the density, based on Hu et al., Eq. 11.
        // If we would violate the floor, then we need to limit the flux. Since the
        // flux adds to the density on one side and subtracts from the other, the floor
        // can only be violated in at most one direction, so we'll do an if-else test
        // below. This means that we can simplify the approach of Hu et al. -- whereas
        // they constructed two thetas for each interface (corresponding to either side)
        // we can complete the operation in one step with a single theta.

        Real drhoL = flux_coefL * flux(i,j,k,URHO);
        Real rhoL = uL[URHO] - drhoL;

        Real drhoR = flux_coefR * flux(i,j,k,URHO);
        Real rhoR = uR[URHO] + drhoR;

        Real theta = 1.0;

        if (rhoL < density_floor) {

            // Obtain the final density corresponding to the LF flux.

            Real drhoLF = flux_coefL * fluxLF[URHO];
            Real rhoLF = uL[URHO] - drhoLF;

            // Solve for theta from (1 - theta) * rhoLF + theta * rho = density_floor.

            theta = amrex::min(theta, (density_floor - rhoLF) / (rhoL - rhoLF));

        }
        else if (rhoR < density_floor) {

            Real drhoLF = flux_coefR * fluxLF[URHO];
            Real rhoLF = uR[URHO] + drhoLF;

            theta = amrex::min(theta, (density_floor - rhoLF) / (rhoR - rhoLF));

        }

        // Limit theta to the valid range (this will deal with roundoff issues).

        theta = amrex::min(1.0, amrex::max(theta, 0.0));

        // Assemble the limited flux (Equation 16).

        for (int n = 0; n < NGDNV; ++n) {
            flux(i,j,k,n) = (1.0 - theta) * fluxLF[n] + theta * flux(i,j,k,n);
        }

        // Now, apply our requirement that the final flux cannot violate the density floor.

        drhoR = flux_coefR * flux(i,j,k,URHO);
        drhoL = flux_coefL * flux(i,j,k,URHO);

        if (uR[URHO] + drhoR < density_floor) {
            for (int n = 0; n < NGDNV; ++n) {
                flux(i,j,k,n) = flux(i,j,k,n) * std::abs((density_floor - uR[URHO]) / drhoR);
            }
        }
        else if (uL[URHO] - drhoL < density_floor) {
            for (int n = 0; n < NGDNV; ++n) {
                flux(i,j,k,n) = flux(i,j,k,n) * std::abs((density_floor - uL[URHO]) / drhoL);
            }
        }

    });
}
