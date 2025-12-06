#!/usr/bin/env python3
"""
bem.py

Pipeline:
  1. Call Gmsh to create a .msh file for N rectangular 3D metal electrodes.
  2. Use Bempp to solve Laplace conduction and extract:
       - Multi-port conductance matrix G [Siemens].
       - Pairwise equivalent resistances between ports with others open.

Model:
  - N metal electrodes (ports), each a rectangular box.
  - Surrounding medium is infinite and homogeneous with conductivity kappa.
  - Laplace equation is solved in the medium:
        ∆V = 0
        V|_port_k = V_k  (Dirichlet BCs)
        V -> 0 at infinity
  - Currents between ports flow through the medium (not inside the metal).
"""

import gmsh
import numpy as np
from scipy.linalg import lu_factor, lu_solve
import bempp_cl
from bempp_cl.api.linalg import gmres
from bempp_cl.api.operators.boundary import sparse

# ------------------------
# User parameters
# ------------------------

NPORTS = 6  # Number of ports (electrodes)
MSH_FILE = "multiport_resistance.msh"

# BEM / material parameters
KAPPA = 1.0  # Conductivity of the medium [S/m]

# Mesh size (global, for simplicity)
LC_MIN = 0.1
LC_MAX = 0.1

# Physical tag offset for ports
PORT_TAG_OFFSET = 100  # port 0 -> 100, port 1 -> 101, etc.


def build_two_layer_cross_mesh(
    msh_filename="two_layer_cross.msh",
    lc_min=0.1,
    lc_max=0.1,
):
    """
    Build the two-layer metal cross + via geometry (6-port) using the Gmsh
    Python API, properly merging touching boxes and removing interior surfaces.
    """

    gmsh.initialize()
    gmsh.model.add("two_layer_cross")

    # ----------------------------------------------------------
    # 1) Mesh options
    # ----------------------------------------------------------
    gmsh.option.setNumber("Mesh.CharacteristicLengthMin", lc_min)
    gmsh.option.setNumber("Mesh.CharacteristicLengthMax", lc_max)
    gmsh.option.setNumber("Mesh.Algorithm", 6)
    gmsh.option.setNumber("Mesh.Smoothing", 50)

    # Set a tolerance for merging (important for touching surfaces)
    gmsh.option.setNumber("Geometry.Tolerance", 1e-8)
    gmsh.option.setNumber("Geometry.ToleranceBoolean", 1e-8)

    occ = gmsh.model.occ

    # ----------------------------------------------------------
    # 2) Create geometry - four boxes
    # ----------------------------------------------------------
    # Bottom X-bar
    v1 = occ.addBox(-0.5, -0.15, 0.0, 1.0, 0.3, 0.2)
    # Bottom Y-bar
    v2 = occ.addBox(-0.15, -0.5, 0.0, 0.3, 1.0, 0.2)
    # Via
    v3 = occ.addBox(-0.075, -0.075, 0.2, 0.15, 0.15, 0.2)
    # Top X-bar
    v4 = occ.addBox(-0.5, -0.15, 0.4, 1.0, 0.3, 0.2)

    # ----------------------------------------------------------
    # 3) First, fragment to combine everything
    # ----------------------------------------------------------
    object_dimtags = [(3, v1), (3, v2), (3, v3), (3, v4)]
    result = occ.fragment(object_dimtags, [])
    occ.synchronize()

    print(f"After fragment: {len(gmsh.model.getEntities(3))} volumes, "
          f"{len(gmsh.model.getEntities(2))} surfaces")

    # ----------------------------------------------------------
    # 4) IMPORTANT: Use booleanUnion to merge touching volumes
    # ----------------------------------------------------------
    # Get all volumes after fragment
    all_vols = gmsh.model.getEntities(3)

    if len(all_vols) > 1:
        # Convert to list of dimtags
        vol_dimtags = [(3, tag) for _, tag in all_vols]

        # Use booleanUnion to merge all volumes into one
        # Start with first volume, union with others one by one
        union_result = vol_dimtags[0:1]  # Start with first volume

        for i in range(1, len(vol_dimtags)):
            # Union current result with next volume
            union_result, _ = occ.fuse(union_result, [vol_dimtags[i]])

        occ.synchronize()

        print(f"After union: {len(gmsh.model.getEntities(3))} volumes, "
              f"{len(gmsh.model.getEntities(2))} surfaces")

    # ----------------------------------------------------------
    # 5) Apply Coherence to remove duplicate surfaces
    # ----------------------------------------------------------
    # Remove duplicate entities (this removes interior faces of merged volumes)
    gmsh.model.occ.removeAllDuplicates()
    occ.synchronize()

    # Get final volumes and surfaces
    all_vols = gmsh.model.getEntities(3)
    all_surfaces = gmsh.model.getEntities(2)

    print(f"After removeAllDuplicates: {len(all_vols)} volumes, "
          f"{len(all_surfaces)} surfaces")

    # Mark the metal volume
    if all_vols:
        gmsh.model.addPhysicalGroup(3, [tag for _, tag in all_vols], 1)
        gmsh.model.setPhysicalName(3, 1, "METAL")

    # ----------------------------------------------------------
    # 6) Identify surfaces for ports
    # ----------------------------------------------------------
    all_surface_tags = [tag for _, tag in all_surfaces]

    # Function to check if surface is in bounding box
    def surface_in_bbox(surf_tag, bbox):
        xmin, ymin, zmin, xmax, ymax, zmax = bbox
        eps = 1e-6
        bbox_surf = gmsh.model.getBoundingBox(2, surf_tag)
        xs, ys, zs, xse, yse, zse = bbox_surf

        return (xs >= xmin - eps and xse <= xmax + eps and ys >= ymin - eps
                and yse <= ymax + eps and zs >= zmin - eps
                and zse <= zmax + eps)

    # Port definitions
    port_definitions = [
        ("PORT_1", 100, (-0.5, -0.15, 0.0, -0.5, 0.15, 0.2)),
        ("PORT_2", 101, (0.5, -0.15, 0.0, 0.5, 0.15, 0.2)),
        ("PORT_3", 102, (-0.15, -0.5, 0.0, 0.15, -0.5, 0.2)),
        ("PORT_4", 103, (-0.15, 0.5, 0.0, 0.15, 0.5, 0.2)),
        ("PORT_5", 104, (-0.5, -0.15, 0.4, -0.5, 0.15, 0.6)),
        ("PORT_6", 105, (0.5, -0.15, 0.4, 0.5, 0.15, 0.6)),
    ]

    port_surface_tags = []

    for name, phys_tag, bbox in port_definitions:
        surfaces = []
        for dim, surf_tag in all_surfaces:
            if surface_in_bbox(surf_tag, bbox):
                surfaces.append(surf_tag)

        if surfaces:
            gmsh.model.addPhysicalGroup(2, surfaces, phys_tag)
            gmsh.model.setPhysicalName(2, phys_tag, name)
            port_surface_tags.extend(surfaces)
            print(f"{name} (tag={phys_tag}): {len(surfaces)} surfaces")
        else:
            print(f"{name} (tag={phys_tag}): No surface")

    # ----------------------------------------------------------
    # 8) Assign remaining surfaces as NON_PORT
    # ----------------------------------------------------------
    non_port_surface_tags = [
        tag for tag in all_surface_tags if tag not in port_surface_tags
    ]

    if non_port_surface_tags:
        gmsh.model.addPhysicalGroup(2, non_port_surface_tags, 1)
        gmsh.model.setPhysicalName(2, 1, "NON_PORT")
        print(f"NON_PORT: {len(non_port_surface_tags)} surfaces")

    # ----------------------------------------------------------
    # 9) Generate mesh
    # ----------------------------------------------------------
    gmsh.model.mesh.generate(2)
    gmsh.write(msh_filename)
    print(f"Mesh written to '{msh_filename}'")

    gmsh.finalize()


# ------------------------
# 3) BEM solve with Bempp
# ------------------------

def build_conductance_matrix(msh_file, nports, kappa, port_tag_offset):
    """
    Build the multi-port conductance matrix G for a conductor with:
      - Dirichlet BC on nports port patches (physical surface tags)
      - Neumann (no-flux) BC on all NON_PORT surfaces.

    The PDE is:
        ∇·(kappa ∇V) = 0 in Ω
        V = prescribed on Γ_p (ports)
        ∂V/∂n = 0 on Γ_N (NON_PORT surfaces)

    The conductance matrix G is defined by:
        For excitation j: V = 1 on port j, 0 on all other ports;
        I_i^(j) = current on port i;   G_ij = I_i^(j).

    Parameters
    ----------
    msh_file : str
        Path to the .msh file (Gmsh output) describing the conductor boundary.
    nports : int
        Number of ports.
    kappa : float
        Conductivity (S/m).
    port_tag_offset : int
        Offset for port tags in the .geo/.msh (e.g. 100 for tags 100..100+nports-1).

    Returns
    -------
    G : (nports, nports) ndarray
        Conductance matrix in Siemens.
    """

    # ----------------------------------------------------------------------
    # 1) Load grid and inspect domain indices (physical surface IDs)
    # ----------------------------------------------------------------------
    print(f"\nStage2: Importing grid from {msh_file}", end="")
    grid = bempp_cl.api.import_grid(msh_file)

    indices = set()
    for e in grid.entity_iterator(0):  # 0 = boundary elements
        indices.add(e.domain_index)
    indices = sorted(indices)

    print("  Number of elements:", grid.number_of_elements)
    print("  Unique domain indices (physical surface IDs):", indices)

    # Prepare port tags (match the ones in .geo)
    port_tags = [port_tag_offset + i for i in range(nports)]

    print("Port tags (physical surface IDs):")
    for i, tag in enumerate(port_tags):
        print(f"  Port {i + 1}: tag = {tag}")

    # Non-port tags (everything else)
    nonport_tags = [tag for tag in indices if tag not in port_tags]
    print("NON_PORT tags:", nonport_tags)

    # ----------------------------------------------------------------------
    # 2) Function space on the FULL boundary (ports + NON_PORT)
    #    Use discontinuous piecewise constants ("DP", 0).
    # ----------------------------------------------------------------------
    space = bempp_cl.api.function_space(grid, "DP", 0)
    ndofs = space.global_dof_count
    print("Function space dimension (full boundary, DP0):", ndofs)

    # ----------------------------------------------------------------------
    # 3) Boundary operators: single-layer (V) and adjoint double-layer (K')
    #    and the "identity" operator used as mass matrix on the boundary.
    # ----------------------------------------------------------------------
    print("\nAssembling boundary operators...")

    slp = bempp_cl.api.operators.boundary.laplace.single_layer(
        space, space, space
    )  # V
    adj_dlp = bempp_cl.api.operators.boundary.laplace.adjoint_double_layer(
        space, space, space
    )  # K'
    ident_op = bempp_cl.api.operators.boundary.sparse.identity(
        space, space, space
    )  # I (mass matrix on boundary for DP0)

    # Get weak forms and convert to dense matrices
    print("  Converting operators to dense matrices...")
    V_mat = slp.weak_form().to_dense()       # Single-layer matrix V
    Kp_mat = adj_dlp.weak_form().to_dense()  # Adjoint double-layer matrix K'
    I_mat = ident_op.weak_form().to_dense()  # Identity (acts as mass matrix)

    print("  V_mat shape:", V_mat.shape)
    print("  Kp_mat shape:", Kp_mat.shape)
    print("  I_mat shape :", I_mat.shape)

    # ----------------------------------------------------------------------
    # 4) Helper callables for Dirichlet data and port indicator functions
    # ----------------------------------------------------------------------
    def dirichlet_fun_factory(excited_port_tag):
        """
        Return callable g(x, n, domain_index, result):
          = 1.0 on excited port,
          = 0.0 on all other surfaces (including other ports and NON_PORT).
        """

        @bempp_cl.api.real_callable
        def g(x, n, domain_index, result):
            if domain_index == excited_port_tag:
                result[0] = 1.0
            else:
                result[0] = 0.0

        return g

    def indicator_fun_factory(port_tag):
        """
        Return callable w(x, n, domain_index, result):
          = 1.0 on given port surface (with given physical tag),
          = 0.0 otherwise.
        Used to integrate quantities over that port.
        """

        @bempp_cl.api.real_callable
        def w(x, n, domain_index, result):
            if domain_index == port_tag:
                result[0] = 1.0
            else:
                result[0] = 0.0

        return w

    # ----------------------------------------------------------------------
    # 5) Build indicator grid functions and deduce DOFs for ports vs NON_PORT
    # ----------------------------------------------------------------------
    print("\nStage3: Building port indicator functions and DOF sets ...")

    port_indicators = []   # w_i as GridFunction on full space
    port_dofs_per_port = []  # list of arrays with DOFs for each port

    for i, tag in enumerate(port_tags):
        w_fun = indicator_fun_factory(tag)
        w_gf = bempp_cl.api.GridFunction(space, fun=w_fun)
        port_indicators.append(w_gf)

        # For DP0, each DOF is associated with one element. This indicator
        # is exactly 1 on elements with domain_index == tag, 0 elsewhere.
        dofs_this_port = np.flatnonzero(np.abs(w_gf.coefficients) > 0.5)
        port_dofs_per_port.append(dofs_this_port)

        print(
            f"  Port {i + 1} (tag {tag}): {dofs_this_port.size} DOFs"
        )

    # Union of all port DOFs
    if port_dofs_per_port:
        port_dofs = np.unique(np.concatenate(port_dofs_per_port))
    else:
        port_dofs = np.array([], dtype=np.int64)

    all_dofs = np.arange(ndofs, dtype=np.int64)
    nonport_dofs = np.setdiff1d(all_dofs, port_dofs)

    print(
        f"Total DOFs : {ndofs}\n"
        f"  Port DOFs: {port_dofs.size}\n"
        f"  NON_PORT DOFs: {nonport_dofs.size}"
    )

    assert port_dofs.size + nonport_dofs.size == ndofs, \
        "Port + NON_PORT DOFs should cover all DOFs."

    # ----------------------------------------------------------------------
    # 6) Assemble the mixed Dirichlet/Neumann block system
    #
    #   On Γ_p (ports):       V φ = g_p
    #   On Γ_N (NON_PORT):    ( 1/2 I + K') φ = 0
    #
    #   With φ = [φ_p; φ_N] in terms of port / NON_PORT DOFs:
    #
    #   [ V_pp        V_pN             ] [φ_p] = [g_p]
    #   [ Kp_Np  Kp_NN + 0.5 * I_NN    ] [φ_N]   [ 0 ]
    #
    # ----------------------------------------------------------------------
    print("\nStage4: Assembling mixed Dirichlet/Neumann block system ...")

    # Convert to arrays for easier indexing
    port_dofs = np.asarray(port_dofs, dtype=int)
    nonport_dofs = np.asarray(nonport_dofs, dtype=int)
    
    # Extract sub-blocks using proper indexing
    # For row and column indexing, use np.ix_ for 2D indexing
    if len(port_dofs) > 0:
        V_pp = V_mat[port_dofs[:, None], port_dofs]
        if len(nonport_dofs) > 0:
            V_pN = V_mat[port_dofs[:, None], nonport_dofs]
        else:
            V_pN = np.zeros((len(port_dofs), 0))
    else:
        V_pp = np.zeros((0, 0))
        V_pN = np.zeros((0, len(nonport_dofs)))
    
    if len(nonport_dofs) > 0:
        if len(port_dofs) > 0:
            Kp_Np = Kp_mat[nonport_dofs[:, None], port_dofs]
        else:
            Kp_Np = np.zeros((len(nonport_dofs), 0))
        Kp_NN = Kp_mat[nonport_dofs[:, None], nonport_dofs]
        I_NN = I_mat[nonport_dofs[:, None], nonport_dofs]
    else:
        Kp_Np = np.zeros((0, len(port_dofs)))
        Kp_NN = np.zeros((0, 0))
        I_NN = np.zeros((0, 0))

    # Build block system matrix A
    if len(port_dofs) > 0 and len(nonport_dofs) > 0:
        A_top = np.hstack([V_pp, V_pN])
        A_bottom = np.hstack([Kp_Np, Kp_NN + 0.5 * I_NN])
        A = np.vstack([A_top, A_bottom])
    elif len(port_dofs) > 0:
        # Only ports, no non-ports
        A = V_pp
    elif len(nonport_dofs) > 0:
        # Only non-ports, no ports (unusual but handle it)
        A = Kp_NN + 0.5 * I_NN
    else:
        raise ValueError("No DOFs found!")

    print("  Block system matrix A shape:", A.shape)

    # Pre-factorize A with dense LU (efficient reuse across excitations)
    print("  Computing dense LU factorization of A ...")
    A_lu = lu_factor(A)
    print("  LU factorization done.")

    # ----------------------------------------------------------------------
    # 7) Solve for each port excitation and build G
    # ----------------------------------------------------------------------
    G = np.zeros((nports, nports), dtype=np.float64)

    for j, exc_tag in enumerate(port_tags):
        print(f"\n=== Solving for excitation of port {j + 1} (tag {exc_tag}) ===")

        # Dirichlet BC: V=1 on port j, V=0 on all other ports.
        g_fun = dirichlet_fun_factory(exc_tag)
        g_dirichlet = bempp_cl.api.GridFunction(space, fun=g_fun)

        # Coefficients of the L2-projection of g onto the DP0 space
        g_coeff_full = g_dirichlet.coefficients.ravel()

        # IMPORTANT: The weak-form single-layer matrix V_mat acts on trial
        # coefficients and produces inner products <ψ_i, S φ>.  The right-hand side
        # on Dirichlet rows must be <ψ_i, g>, not the projection coefficients
        # themselves.  Since I_mat is the L2 mass matrix, we have:
        #     rhs_full = I_mat * g_coeff_full  ≈ [ <ψ_i, g> ]_i
        g_rhs_full = I_mat @ g_coeff_full

        # Restrict Dirichlet RHS to port test dofs
        g_p = np.asarray(g_rhs_full).ravel()[port_dofs]

        zeros_N = np.zeros(len(nonport_dofs), dtype=np.float64)
        rhs = np.concatenate([g_p, zeros_N])

        # Solve A * [φ_p; φ_N] = rhs using dense LU
        phi_vec = lu_solve(A_lu, rhs)

        n_p = len(port_dofs)
        n_N = len(nonport_dofs)
        assert phi_vec.size == n_p + n_N

        phi_p = phi_vec[:n_p]
        phi_N = phi_vec[n_p:]

        # Assemble full density φ on all boundary DOFs
        phi_coeff = np.zeros(ndofs, dtype=np.float64)
        phi_coeff[port_dofs] = phi_p
        phi_coeff[nonport_dofs] = phi_N

        phi_j = bempp_cl.api.GridFunction(space, coefficients=phi_coeff)

        # ------------------------------------------------------------------
        # Compute weak normal derivative and port currents:
        #
        #   q_int = ∂V/∂n_int = (0.5 I + K') φ   (strong form)
        #
        # In weak form (with DP0 test functions ψ_i),
        #   (q_weak)_i = ∫ ψ_i q_int dS
        #              = ∑_j (Kp_mat + 0.5 * I_mat)_{ij} * φ_j
        #
        # We never need the strong coefficients of q_int; for the currents we
        # only need ∫_{Γ_i} q_int dS, which is just a sum over q_weak entries
        # on port i. This keeps everything consistent with the block system,
        # which also uses (Kp_mat - 0.5 I_mat).
        # ------------------------------------------------------------------
        print("  Computing port currents from weak flux q_weak = (K' - 0.5 I) φ ...")

        # Full weak flux vector: length = ndofs
        # q_weak[k] ≈ ∫ ψ_k(x) q_int(x) dS_x
        q_weak = (Kp_mat + 0.5 * I_mat) @ phi_coeff
        q_weak = np.asarray(q_weak).ravel()

        # Integrate q_int over each port using the indicator functions.
        # For DP0, each w_i has coefficients equal to 1 on elements in Γ_i,
        # 0 elsewhere, so
        #   ∫_{Γ_i} q_int dS ≈ w_i^T * q_weak
        for i, w_i in enumerate(port_indicators):
            w_coeff = w_i.coefficients.ravel()

            flux_int = float(w_coeff @ q_weak)
            I_ij = -kappa * flux_int
            G[i, j] = I_ij

            print(
                f"    Port {i + 1} current for excitation {j + 1}: "
                f"I[{i + 1},{j + 1}] = {I_ij:.6e} A"
            )

    # ----------------------------------------------------------------------
    # 8) Post-process signs and check row/column sums
    # ----------------------------------------------------------------------
    # For a passive resistive network, we usually want G_ii < 0 and sum_j G_ij = 0.
    if nports > 0 and G[0, 0] > 0:
        print(
            "\nDiagonal of G is positive; flipping overall sign for conventional form."
        )
        G = -G

    row_sums = G.sum(axis=1)
    col_sums = G.sum(axis=0)

    print("\nRow sums of G (should be ~0):", row_sums)
    print("Col sums of G (should be ~0):", col_sums)

    return G


# ------------------------
# 4) Convert conductance matrix to pairwise resistances
# ------------------------


def equivalent_resistance(G, a, b):
    """
    Compute the equivalent resistance R_ab between ports
    'a' and 'b' (0-based indices), with all other ports open-circuited.

    We solve:
      - V_a = +1
      - V_b = 0
      - For k != a, b: I_k = 0  (open-circuit)
      - I = G V

    Returns:
      R_ab (float)
    """
    n = G.shape[0]
    all_indices = list(range(n))
    free = [k for k in all_indices if k not in (a, b)]

    # If there are no other ports, it's trivial: I_a = G_aa * 1 + G_ab * 0
    if len(free) == 0:
        I_a = G[a, a]
        return 1.0 / I_a

    G_ff = G[np.ix_(free, free)]
    G_fa = G[np.ix_(free, [a])]  # column vector
    G_fb = G[np.ix_(free, [b])]  # column vector

    Va = 1.0
    Vb = 0.0

    # Open-circuit at free ports: I_free = G_ff V_free + G_fa Va + G_fb Vb = 0
    rhs = -(G_fa * Va + G_fb * Vb).ravel()
    V_free = np.linalg.solve(G_ff, rhs)

    V = np.zeros(n)
    V[a] = Va
    V[b] = Vb
    for idx, val in zip(free, V_free):
        V[idx] = val

    I = G.dot(V)
    I_a = I[a]
    R_ab = (Va - Vb) / I_a
    return R_ab


def print_pairwise_resistances(G):
    """
    Print R_ab for all port pairs a < b.
    """
    n = G.shape[0]
    print("\nPairwise equivalent resistances (others open-circuit):")
    for a in range(n):
        for b in range(a + 1, n):
            R_ab = equivalent_resistance(G, a, b)
            print(f"  R({a+1},{b+1}) = {R_ab:.6e} ohm")


# ------------------------
# Main
# ------------------------


def main():
    # 1) Write .geo
    build_two_layer_cross_mesh(MSH_FILE, LC_MIN, LC_MAX)

    # 2) Build conductance matrix via BEM
    G = build_conductance_matrix(MSH_FILE, NPORTS, KAPPA, PORT_TAG_OFFSET)

    print("\nConductance matrix G [Siemens]:")
    np.set_printoptions(precision=4, suppress=True)
    print(G)

    # 3) Print pairwise resistances
    print_pairwise_resistances(G)

    # 4) Save matrices to disk
    np.savetxt("G_matrix_Siemens.txt", G)
    print("\nSaved G_matrix_Siemens.txt")


if __name__ == "__main__":
    main()
