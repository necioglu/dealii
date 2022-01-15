// ---------------------------------------------------------------------
//
// Copyright (C) 2000 - 2020 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------

#ifndef dealii_fe_tools_extrapolate_templates_H
#define dealii_fe_tools_extrapolate_templates_H


#include <deal.II/base/config.h>

#include <deal.II/base/mpi_consensus_algorithms.h>

#include <deal.II/distributed/p4est_wrappers.h>
#include <deal.II/distributed/tria.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_tools.h>
#include <deal.II/fe/fe_tools_interpolate.templates.h>

#include <deal.II/grid/tria.h>

#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/block_vector.h>
#include <deal.II/lac/la_parallel_block_vector.h>
#include <deal.II/lac/la_parallel_vector.h>
#include <deal.II/lac/la_vector.h>
#include <deal.II/lac/petsc_block_vector.h>
#include <deal.II/lac/petsc_vector.h>
#include <deal.II/lac/trilinos_parallel_block_vector.h>
#include <deal.II/lac/trilinos_vector.h>

#include <queue>

DEAL_II_NAMESPACE_OPEN

namespace FETools
{
  namespace internal
  {
#ifndef DEAL_II_WITH_P4EST
    // Dummy implementation in case p4est is not available.
    template <int dim, int spacedim, class OutVector>
    class ExtrapolateImplementation
    {
    public:
      ExtrapolateImplementation()
      {
        Assert(false, ExcNotImplemented());
      };

      template <class InVector>
      void
      extrapolate_parallel(const InVector & /*u2_relevant*/,
                           const DoFHandler<dim, spacedim> & /*dof2*/,
                           OutVector & /*u2*/)
      {
        Assert(false, ExcNotImplemented())
      }
    };

#else

    /**
     * Implementation of the FETools::extrapolate() function
     * on parallel distributed grids.
     */
    template <int dim, int spacedim, class OutVector>
    class ExtrapolateImplementation
    {
    public:
      ExtrapolateImplementation();

      template <class InVector>
      void
      extrapolate_parallel(const InVector &                 u2_relevant,
                           const DoFHandler<dim, spacedim> &dof2,
                           OutVector &                      u2);

    private:
      /**
       *  A shortcut for the type of the OutVector.
       */
      using value_type = typename OutVector::value_type;

      /**
       * A structure holding all data to set dofs recursively on cells of
       * arbitrary level.
       */
      struct WorkPackage
      {
        const typename dealii::internal::p4est::types<dim>::forest forest;
        const typename dealii::internal::p4est::types<dim>::tree   tree;
        const typename dealii::internal::p4est::types<dim>::locidx tree_index;
        const typename DoFHandler<dim, spacedim>::cell_iterator    dealii_cell;
        const typename dealii::internal::p4est::types<dim>::quadrant p4est_cell;
      };


      /**
       * A structure holding all data of cells needed from other processes
       * for the extrapolate algorithm.
       */
      struct CellData
      {
        /**
         * Default constructor.
         */
        CellData();

        /**
         * Constructor setting the `dof_values` member to the right size.
         */
        CellData(const unsigned int dofs_per_cell);

        /**
         * Comparison operator.
         */
        bool
        operator<(const CellData &rhs) const;

        /**
         * Pack the data of this object into a buffer.
         */
        std::vector<char>
        pack_data() const;

        /**
         * Unpack the data of the given buffer into the members of this object.
         */
        void
        unpack_data(const std::vector<char> &buffer);


        /**
         * The values of degrees of freedom associated with the current cell.
         */
        Vector<value_type> dof_values;

        /**
         * The tree within the forest (i.e., the coarse cell) and which of its
         * descendents we are currently working on.
         */
        unsigned int                                           tree_index;
        typename dealii::internal::p4est::types<dim>::quadrant quadrant;

        types::subdomain_id receiver;
      };

      // Problem: The function extrapolates a polynomial
      // function from a finer mesh of size $h$ to a polynmial
      // function of higher degree but on a coarser mesh of
      // size $2h$. Therefore the mesh has to consist of patches
      // of four (2d) or eight (3d) cells and the function requires
      // that the mesh is refined globally at least once.
      // The algorithm starts on the coarsest level of the grid,
      // loops over all cells and if a cell has at least one active child,
      // dof values are set via
      //   cell->get_interpolated_dof_values(u_input, dof_values)
      //   cell->set_dof_values_by_interpolation(dof_values, u_output)
      // both *_interpolation_* functions traverse recursively
      // over all children down to the active cells
      // and get/set dof values by interpolation.
      //
      // On distributed parallel grids the problem arises, that
      // if a cell has at least one active child, there is no guarantee
      // that all children of this cell belong to this process.
      // There might be children which are owned by other processes
      // and the algorithm needs to find and has to get the
      // dof values from these processes and so on.
      //
      // Algorithm:
      // 1) Loop over all coarse cells
      //      From each coarse cell traverse down the tree
      //      of refined cells and search for active children
      //      If there is an active child, check, if all
      //      other children down to the finest level are part
      //      of this process, if not, add the cell to the list
      //      of needs.
      // 2) Send the list of needs to other processes
      //      Each process has a list of needs and after
      //      the loop over all coarse cells (all trees)
      //      is finished, this list can be send to other processes.
      // 3) Compute the needs required by other processes
      //      While computing the values required from other
      //      processes there can arise new needs and they are
      //      stored in a list again.
      // 4) Send the computed values and the list of new needs around
      //
      // This procedure has to be repeated until no process needs any
      // new need and all needs are computed, but there are at most the
      // "number of grid levels" rounds of sending/receiving cell data.
      //
      // Then each process has all data needed for extrapolation.

      // driver function sending/receiving all values from
      // different processes
      template <class InVector>
      void
      compute_all_non_local_data(const DoFHandler<dim, spacedim> &dof2,
                                 const InVector &                 u);

      // traverse recursively over
      // the cells of this tree and
      // interpolate over patches which
      // are part of this process
      template <class InVector>
      void
      interpolate_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &             p4est_cell,
        const InVector &u1,
        OutVector &     u2);

      // get dof values for this
      // cell by interpolation
      // if a child is reached which
      // is not part of this process
      // a new need is created
      template <class InVector>
      void
      get_interpolated_dof_values(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        const InVector &       u,
        Vector<value_type> &   interpolated_values,
        std::vector<CellData> &new_needs);

      // set dof values for this
      // cell by interpolation
      void
      set_dof_values_by_interpolation(
        const typename DoFHandler<dim, spacedim>::cell_iterator &dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                       p4est_cell,
        const Vector<value_type> &interpolated_values,
        OutVector &               u);

      // compute all cell_data
      // needed from other processes
      // to interpolate the part of
      // this process
      void
      compute_needs(const DoFHandler<dim, spacedim> &dof2,
                    std::vector<CellData> &          new_needs);

      // traverse over the tree
      // and look for patches this
      // process has to work on
      void
      traverse_tree_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        std::vector<CellData> &new_needs);

      // traverse recursively
      // over a patch and look
      // for cells needed from
      // other processes for interpolation
      void
      traverse_patch_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        std::vector<CellData> &new_needs);

      // compute dof values of all
      // cells collected in cells_to_compute
      // computed cells are deleted
      // from cells_to_compute and
      // stored in computed_cells
      // during computation there can
      // be new cells needed from
      // other processes, these cells
      // are stored in new_needs
      template <class InVector>
      void
      compute_cells(const DoFHandler<dim, spacedim> &dof2,
                    const InVector &                 u,
                    std::vector<CellData> &          cells_to_compute,
                    std::vector<CellData> &          computed_cells,
                    std::vector<CellData> &          new_needs);

      // traverse over the tree
      // and compute cells
      template <class InVector>
      void
      compute_cells_in_tree_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        const InVector &       u,
        std::vector<CellData> &cells_to_compute,
        std::vector<CellData> &computed_cells,
        std::vector<CellData> &new_needs);

      // send all cell_data in the vector
      // cells_to_send to their receivers
      // and receives a vector of cell_data
      void
      exchange_data_on_cells(const std::vector<CellData> &cells_to_send,
                             std::vector<CellData> &      received_cells) const;

      // add new cell_data to
      // the ordered list new_needs
      // uses cell_data_insert
      static void
      add_new_need(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        std::vector<CellData> &new_needs);

      // binary search in cells_list
      // assume that cells_list
      // is ordered
      static int
      cell_data_search(const CellData &             cell_data,
                       const std::vector<CellData> &cells_list);

      // insert cell_data into a sorted
      // vector cells_list at the correct
      // position if cell_data
      // not exists already in cells_list
      static void
      cell_data_insert(const CellData &       cell_data,
                       std::vector<CellData> &cells_list);

      MPI_Comm communicator;

      // a vector of all cells this process has
      // computed or received data
      std::vector<CellData> available_cells;

      // stores the indices of dofs on more refined ghosted cells along
      // with the maximum level
      std::map<types::global_dof_index, int> dofs_on_refined_neighbors;

      // counts the send/receive round we are in
      unsigned int round;
    };



    template <class OutVector>
    class ExtrapolateImplementation<1, 1, OutVector>
    {
    public:
      ExtrapolateImplementation()
      {
        AssertThrow(false, ExcNotImplemented())
      }

      template <class InVector>
      void
      extrapolate_parallel(const InVector & /*u2_relevant*/,
                           const DoFHandler<1, 1> & /*dof2*/,
                           OutVector & /*u2*/)
      {}
    };

    template <class OutVector>
    class ExtrapolateImplementation<1, 2, OutVector>
    {
    public:
      ExtrapolateImplementation()
      {
        AssertThrow(false, ExcNotImplemented())
      }

      template <class InVector>
      void
      extrapolate_parallel(const InVector & /*u2_relevant*/,
                           const DoFHandler<1, 2> & /*dof2*/,
                           OutVector & /*u2*/)
      {}
    };

    template <class OutVector>
    class ExtrapolateImplementation<1, 3, OutVector>
    {
    public:
      ExtrapolateImplementation()
      {
        AssertThrow(false, ExcNotImplemented())
      }

      template <class InVector>
      void
      extrapolate_parallel(const InVector & /*u2_relevant*/,
                           const DoFHandler<1, 3> & /*dof2*/,
                           OutVector & /*u2*/)
      {}
    };



    template <int dim, int spacedim, class OutVector>
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      ExtrapolateImplementation()
      : round(0)
    {}



    template <int dim, int spacedim, class OutVector>
    ExtrapolateImplementation<dim, spacedim, OutVector>::CellData::CellData()
      : tree_index(0)
      , receiver(0)
    {}



    template <int dim, int spacedim, class OutVector>
    ExtrapolateImplementation<dim, spacedim, OutVector>::CellData::CellData(
      const unsigned int dofs_per_cell)
      : dof_values(dofs_per_cell)
      , tree_index(0)
      , receiver(0)
    {}



    template <int dim, int spacedim, class OutVector>
    bool
    ExtrapolateImplementation<dim, spacedim, OutVector>::CellData::operator<(
      const CellData &rhs) const
    {
      if (dealii::internal::p4est::functions<dim>::quadrant_compare(
            &quadrant, &rhs.quadrant) < 0)
        return true;

      return false;
    }



    template <int dim, int spacedim, class OutVector>
    std::vector<char>
    ExtrapolateImplementation<dim, spacedim, OutVector>::CellData::pack_data()
      const
    {
      // Compute how much memory we need to pack the data of this
      // structure into a char[] buffer.
      const unsigned int bytes_for_buffer =
        (sizeof(unsigned int) +                   // dofs_per_cell
         dof_values.size() * sizeof(value_type) + // dof_values
         sizeof(unsigned int) +                   // tree_index
         sizeof(
           typename dealii::internal::p4est::types<dim>::quadrant)); // quadrant
      std::vector<char> buffer(bytes_for_buffer);

      char *ptr = buffer.data();

      unsigned int n_dofs = dof_values.size();
      std::memcpy(ptr, &n_dofs, sizeof(unsigned int));
      ptr += sizeof(unsigned int);

      std::memcpy(ptr, dof_values.begin(), n_dofs * sizeof(value_type));
      ptr += n_dofs * sizeof(value_type);

      std::memcpy(ptr, &tree_index, sizeof(unsigned int));
      ptr += sizeof(unsigned int);

      std::memcpy(ptr,
                  &quadrant,
                  sizeof(
                    typename dealii::internal::p4est::types<dim>::quadrant));
      ptr += sizeof(typename dealii::internal::p4est::types<dim>::quadrant);

      Assert(ptr == buffer.data() + buffer.size(), ExcInternalError());

      return buffer;
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::CellData::unpack_data(
      const std::vector<char> &buffer)
    {
      const char * ptr = buffer.data();
      unsigned int n_dofs;
      memcpy(&n_dofs, ptr, sizeof(unsigned int));
      ptr += sizeof(unsigned int);

      dof_values.reinit(n_dofs);
      std::memcpy(dof_values.begin(), ptr, n_dofs * sizeof(value_type));
      ptr += n_dofs * sizeof(value_type);

      std::memcpy(&tree_index, ptr, sizeof(unsigned int));
      ptr += sizeof(unsigned int);

      std::memcpy(&quadrant,
                  ptr,
                  sizeof(
                    typename dealii::internal::p4est::types<dim>::quadrant));
      ptr += sizeof(typename dealii::internal::p4est::types<dim>::quadrant);

      Assert(ptr == buffer.data() + buffer.size(), ExcInternalError());
    }



    template <int dim, int spacedim, class OutVector>
    template <class InVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      interpolate_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &             p4est_cell,
        const InVector &u1,
        OutVector &     u2)
    {
      // check if this cell exists in the local p4est
      int idx = sc_array_bsearch(
        const_cast<sc_array_t *>(&tree.quadrants),
        &p4est_cell,
        dealii::internal::p4est::functions<dim>::quadrant_compare);

      // if neither this cell nor one of it's children belongs to us, don't do
      // anything
      if (idx == -1 &&
          (dealii::internal::p4est::functions<dim>::quadrant_overlaps_tree(
             const_cast<typename dealii::internal::p4est::types<dim>::tree *>(
               &tree),
             &p4est_cell) == false))
        return;

      bool p4est_has_children = (idx == -1);

      bool locally_owned_children = false;
      if (p4est_has_children)
        {
          Assert(dealii_cell->has_children(), ExcInternalError());

          // check if at least one child is locally owned on our process
          for (unsigned int child_n = 0; child_n < dealii_cell->n_children();
               ++child_n)
            if (dealii_cell->child(child_n)->is_active())
              if (dealii_cell->child(child_n)->is_locally_owned())
                {
                  locally_owned_children = true;
                  break;
                }
        }

      if (locally_owned_children)
        {
          const FiniteElement<dim, spacedim> &fe =
            dealii_cell->get_dof_handler().get_fe();
          const unsigned int dofs_per_cell = fe.n_dofs_per_cell();

          Vector<typename OutVector::value_type> interpolated_values(
            dofs_per_cell);

          std::vector<CellData> new_needs;
          get_interpolated_dof_values(forest,
                                      tree,
                                      tree_index,
                                      dealii_cell,
                                      p4est_cell,
                                      u1,
                                      interpolated_values,
                                      new_needs);

          // at this point of
          // the procedure no new
          // needs should come up
          Assert(new_needs.size() == 0, ExcInternalError());

          set_dof_values_by_interpolation(dealii_cell,
                                          p4est_cell,
                                          interpolated_values,
                                          u2);
        }
    }



    template <int dim, int spacedim, class OutVector>
    template <class InVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      get_interpolated_dof_values(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        const InVector &       u,
        Vector<value_type> &   interpolated_values,
        std::vector<CellData> &new_needs)
    {
      if (dealii_cell->is_active())
        {
          if (dealii_cell->is_locally_owned())
            {
              // if this is one of our cells,
              // get dof values from input vector
              dealii_cell->get_dof_values(u, interpolated_values);
            }
          else
            {
              add_new_need(
                forest, tree_index, dealii_cell, p4est_cell, new_needs);
            }
        }
      else
        {
          const FiniteElement<dim, spacedim> &fe =
            dealii_cell->get_dof_handler().get_fe();
          const unsigned int dofs_per_cell = fe.n_dofs_per_cell();

          Assert(interpolated_values.size() == dofs_per_cell,
                 ExcDimensionMismatch(interpolated_values.size(),
                                      dofs_per_cell));
          Assert(u.size() == dealii_cell->get_dof_handler().n_dofs(),
                 ExcDimensionMismatch(u.size(),
                                      dealii_cell->get_dof_handler().n_dofs()));

          Vector<value_type> tmp1(dofs_per_cell);
          Vector<value_type> tmp2(dofs_per_cell);

          interpolated_values = 0;
          std::vector<bool> restriction_is_additive(dofs_per_cell);
          for (unsigned int i = 0; i < dofs_per_cell; ++i)
            restriction_is_additive[i] = fe.restriction_is_additive(i);

          typename dealii::internal::p4est::types<dim>::quadrant
            p4est_child[GeometryInfo<dim>::max_children_per_cell];

          dealii::internal::p4est::init_quadrant_children<dim>(p4est_cell,
                                                               p4est_child);

          bool found_child = true;
          for (unsigned int c = 0; c < GeometryInfo<dim>::max_children_per_cell;
               ++c)
            {
              if (dealii::internal::p4est::functions<dim>::
                    quadrant_overlaps_tree(
                      const_cast<
                        typename dealii::internal::p4est::types<dim>::tree *>(
                        &tree),
                      &p4est_child[c]) == false)
                {
                  // this is a cell this process needs
                  // data from another process

                  // check if this cell is
                  // available from other
                  // computed patches
                  CellData cell_data;
                  cell_data.quadrant = p4est_child[c];
                  int pos = cell_data_search(cell_data, available_cells);

                  if (pos == -1)
                    {
                      // data is not available
                      // create a new need
                      found_child = false;

                      add_new_need(forest,
                                   tree_index,
                                   dealii_cell->child(c),
                                   p4est_child[c],
                                   new_needs);
                    }
                  else
                    {
                      Assert(available_cells[pos].dof_values.size() ==
                               dofs_per_cell,
                             ExcDimensionMismatch(
                               available_cells[pos].dof_values.size(),
                               dofs_per_cell));

                      tmp1 = available_cells[pos].dof_values;
                    }
                }
              else
                {
                  // get the values from the present child, if necessary by
                  // interpolation either from its own children or
                  // by interpolating from the finite element on an active
                  // child to the finite element space requested here
                  get_interpolated_dof_values(forest,
                                              tree,
                                              tree_index,
                                              dealii_cell->child(c),
                                              p4est_child[c],
                                              u,
                                              tmp1,
                                              new_needs);
                }

              if (found_child)
                {
                  // interpolate these to the mother cell
                  fe.get_restriction_matrix(c, dealii_cell->refinement_case())
                    .vmult(tmp2, tmp1);

                  // and add up or set them in the output vector
                  for (unsigned int i = 0; i < dofs_per_cell; ++i)
                    if (tmp2(i) != value_type())
                      {
                        if (restriction_is_additive[i])
                          interpolated_values(i) += tmp2(i);
                        else
                          interpolated_values(i) = tmp2(i);
                      }
                }
            }

          if (found_child == false)
            interpolated_values = 0;
        }
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      set_dof_values_by_interpolation(
        const typename DoFHandler<dim, spacedim>::cell_iterator &dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                       p4est_cell,
        const Vector<value_type> &local_values,
        OutVector &               u)
    {
      const FiniteElement<dim, spacedim> &fe =
        dealii_cell->get_dof_handler().get_fe();
      const unsigned int dofs_per_cell = fe.n_dofs_per_cell();

      if (dealii_cell->is_active())
        {
          if (dealii_cell->is_locally_owned())
            {
              std::vector<types::global_dof_index> indices(dofs_per_cell);
              dealii_cell->get_dof_indices(indices);
              for (unsigned int j = 0; j < dofs_per_cell; ++j)
                {
                  // don't set this dof if there is a more refined ghosted
                  // neighbor setting this dof.
                  const bool on_refined_neighbor =
                    (dofs_on_refined_neighbors.find(indices[j]) !=
                     dofs_on_refined_neighbors.end());
                  if (!(on_refined_neighbor &&
                        dofs_on_refined_neighbors[indices[j]] >
                          dealii_cell->level()))
                    ::dealii::internal::ElementAccess<OutVector>::set(
                      local_values(j), indices[j], u);
                }
            }
        }
      else
        {
          Assert(local_values.size() == dofs_per_cell,
                 ExcDimensionMismatch(local_values.size(), dofs_per_cell));
          Assert(u.size() == dealii_cell->get_dof_handler().n_dofs(),
                 ExcDimensionMismatch(u.size(),
                                      dealii_cell->get_dof_handler().n_dofs()));

          Vector<value_type> tmp(dofs_per_cell);

          typename dealii::internal::p4est::types<dim>::quadrant
            p4est_child[GeometryInfo<dim>::max_children_per_cell];

          dealii::internal::p4est::init_quadrant_children<dim>(p4est_cell,
                                                               p4est_child);

          for (unsigned int c = 0; c < GeometryInfo<dim>::max_children_per_cell;
               ++c)
            {
              if (tmp.size() > 0)
                fe.get_prolongation_matrix(c, dealii_cell->refinement_case())
                  .vmult(tmp, local_values);

              set_dof_values_by_interpolation(dealii_cell->child(c),
                                              p4est_child[c],
                                              tmp,
                                              u);
            }
        }
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::compute_needs(
      const DoFHandler<dim, spacedim> &dof2,
      std::vector<CellData> &          new_needs)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *tr =
        (dynamic_cast<const parallel::distributed::Triangulation<dim, spacedim>
                        *>(&dof2.get_triangulation()));

      Assert(tr != nullptr, ExcInternalError());

      for (const auto &cell : dof2.cell_iterators_on_level(0))
        {
          if (dealii::internal::p4est::tree_exists_locally<dim>(
                tr->parallel_forest,
                tr->coarse_cell_to_p4est_tree_permutation[cell->index()]) ==
              false)
            continue;

          typename dealii::internal::p4est::types<dim>::quadrant
                             p4est_coarse_cell;
          const unsigned int tree_index =
            tr->coarse_cell_to_p4est_tree_permutation[cell->index()];
          typename dealii::internal::p4est::types<dim>::tree *tree =
            tr->init_tree(cell->index());

          dealii::internal::p4est::init_coarse_quadrant<dim>(p4est_coarse_cell);

          // make sure that each cell on the
          // coarsest level is at least once
          // refined, otherwise, these cells
          // can't be treated and would
          // generate a bogus result
          {
            int idx = sc_array_bsearch(
              const_cast<sc_array_t *>(&tree->quadrants),
              &p4est_coarse_cell,
              dealii::internal::p4est::functions<dim>::quadrant_compare);

            AssertThrow(idx == -1, ExcGridNotRefinedAtLeastOnce());
          }

          traverse_tree_recursively(*tr->parallel_forest,
                                    *tree,
                                    tree_index,
                                    cell,
                                    p4est_coarse_cell,
                                    new_needs);
        }
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      traverse_tree_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        std::vector<CellData> &new_needs)
    {
      // check if this cell exists in the local p4est
      int idx = sc_array_bsearch(
        const_cast<sc_array_t *>(&tree.quadrants),
        &p4est_cell,
        dealii::internal::p4est::functions<dim>::quadrant_compare);

      // if neither this cell nor one of it's children belongs to us, don't do
      // anything
      if (idx == -1 &&
          (dealii::internal::p4est::functions<dim>::quadrant_overlaps_tree(
             const_cast<typename dealii::internal::p4est::types<dim>::tree *>(
               &tree),
             &p4est_cell) == false))
        return;

      bool p4est_has_children = (idx == -1);

      // this cell is part of a patch
      // this process has to interpolate on
      // if there is at least one locally
      // owned child
      bool locally_owned_children = false;
      if (p4est_has_children)
        {
          Assert(dealii_cell->has_children(), ExcInternalError());

          for (unsigned int child_n = 0; child_n < dealii_cell->n_children();
               ++child_n)
            if (dealii_cell->child(child_n)->is_active())
              if (dealii_cell->child(child_n)->is_locally_owned())
                {
                  locally_owned_children = true;
                  break;
                }
        }

      // traverse the patch recursively
      // to find cells needed from
      // other processes
      if (locally_owned_children)
        {
          traverse_patch_recursively(
            forest, tree, tree_index, dealii_cell, p4est_cell, new_needs);
        }

      // traverse recursively over this tree
      if (p4est_has_children)
        {
          typename dealii::internal::p4est::types<dim>::quadrant
            p4est_child[GeometryInfo<dim>::max_children_per_cell];

          dealii::internal::p4est::init_quadrant_children<dim>(p4est_cell,
                                                               p4est_child);

          for (unsigned int c = 0; c < GeometryInfo<dim>::max_children_per_cell;
               ++c)
            {
              traverse_tree_recursively(forest,
                                        tree,
                                        tree_index,
                                        dealii_cell->child(c),
                                        p4est_child[c],
                                        new_needs);
            }
        }
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      traverse_patch_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        std::vector<CellData> &new_needs)
    {
      if (dealii_cell->has_children())
        {
          Assert(dealii_cell->has_children(), ExcInternalError());

          typename dealii::internal::p4est::types<dim>::quadrant
            p4est_child[GeometryInfo<dim>::max_children_per_cell];

          dealii::internal::p4est::init_quadrant_children<dim>(p4est_cell,
                                                               p4est_child);

          for (unsigned int c = 0; c < GeometryInfo<dim>::max_children_per_cell;
               ++c)
            {
              // check if this child
              // is locally available
              // in the p4est
              if (dealii::internal::p4est::functions<dim>::
                    quadrant_overlaps_tree(
                      const_cast<
                        typename dealii::internal::p4est::types<dim>::tree *>(
                        &tree),
                      &p4est_child[c]) == false)
                {
                  // this is a cell for which this process
                  // needs data from another process
                  // so add the cell to the list
                  add_new_need(forest,
                               tree_index,
                               dealii_cell->child(c),
                               p4est_child[c],
                               new_needs);
                }
              else
                {
                  // at least some part of
                  // the tree rooted in this
                  // child is locally available
                  traverse_patch_recursively(forest,
                                             tree,
                                             tree_index,
                                             dealii_cell->child(c),
                                             p4est_child[c],
                                             new_needs);
                }
            }
        }
    }



    template <int dim, int spacedim, class OutVector>
    template <class InVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::compute_cells(
      const DoFHandler<dim, spacedim> &dof2,
      const InVector &                 u,
      std::vector<CellData> &          cells_to_compute,
      std::vector<CellData> &          computed_cells,
      std::vector<CellData> &          new_needs)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *tr =
        (dynamic_cast<const parallel::distributed::Triangulation<dim, spacedim>
                        *>(&dof2.get_triangulation()));

      Assert(tr != nullptr, ExcInternalError());

      // collect in a set all trees this
      // process has to compute cells on
      std::set<unsigned int> trees;
      for (const auto &c : cells_to_compute)
        trees.insert(c.tree_index);

      for (const auto &cell : dof2.cell_iterators_on_level(0))
        {
          // check if this is a tree this process has to
          // work on and that this tree is in the p4est
          const unsigned int tree_index =
            tr->coarse_cell_to_p4est_tree_permutation[cell->index()];

          if ((trees.find(tree_index) == trees.end()) ||
              (dealii::internal::p4est::tree_exists_locally<dim>(
                 tr->parallel_forest, tree_index) == false))
            continue;

          typename dealii::internal::p4est::types<dim>::quadrant
                                                              p4est_coarse_cell;
          typename dealii::internal::p4est::types<dim>::tree *tree =
            tr->init_tree(cell->index());

          dealii::internal::p4est::init_coarse_quadrant<dim>(p4est_coarse_cell);

          compute_cells_in_tree_recursively(*tr->parallel_forest,
                                            *tree,
                                            tree_index,
                                            cell,
                                            p4est_coarse_cell,
                                            u,
                                            cells_to_compute,
                                            computed_cells,
                                            new_needs);
        }
    }



    template <int dim, int spacedim, class OutVector>
    template <class InVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      compute_cells_in_tree_recursively(
        const typename dealii::internal::p4est::types<dim>::forest &forest,
        const typename dealii::internal::p4est::types<dim>::tree &  tree,
        const typename dealii::internal::p4est::types<dim>::locidx &tree_index,
        const typename DoFHandler<dim, spacedim>::cell_iterator &   dealii_cell,
        const typename dealii::internal::p4est::types<dim>::quadrant
          &                    p4est_cell,
        const InVector &       u,
        std::vector<CellData> &cells_to_compute,
        std::vector<CellData> &computed_cells,
        std::vector<CellData> &new_needs)
    {
      if (cells_to_compute.size() == 0)
        return;

      // check if this cell exists in the local p4est
      int idx = sc_array_bsearch(
        const_cast<sc_array_t *>(&tree.quadrants),
        &p4est_cell,
        dealii::internal::p4est::functions<dim>::quadrant_compare);

      // if neither this cell nor one of it's children belongs to us, don't do
      // anything
      if (idx == -1 &&
          (dealii::internal::p4est::functions<dim>::quadrant_overlaps_tree(
             const_cast<typename dealii::internal::p4est::types<dim>::tree *>(
               &tree),
             &p4est_cell) == false))
        return;

      bool p4est_has_children = (idx == -1);

      // check if this quadrant is in the list
      CellData cell_data;
      cell_data.quadrant = p4est_cell;
      int pos            = cell_data_search(cell_data, cells_to_compute);
      if (pos != -1)
        {
          std::vector<CellData> tmp;
          // compute dof values
          get_interpolated_dof_values(forest,
                                      tree,
                                      tree_index,
                                      dealii_cell,
                                      p4est_cell,
                                      u,
                                      cells_to_compute[pos].dof_values,
                                      tmp);
          // there is no new cell_data
          // this process needs for computing this cell,
          // store cell_data in the list of
          // computed cells and erase this cell
          // from the list of cells to compute
          if (tmp.size() == 0)
            {
              cell_data_insert(cells_to_compute[pos], computed_cells);
              cells_to_compute.erase(cells_to_compute.begin() + pos);
            }
          else
            {
              for (unsigned int i = 0; i < tmp.size(); ++i)
                cell_data_insert(tmp[i], new_needs);
            }
        }

      // search recursively over this tree
      if (p4est_has_children)
        {
          typename dealii::internal::p4est::types<dim>::quadrant
            p4est_child[GeometryInfo<dim>::max_children_per_cell];

          dealii::internal::p4est::init_quadrant_children<dim>(p4est_cell,
                                                               p4est_child);

          for (unsigned int c = 0; c < GeometryInfo<dim>::max_children_per_cell;
               ++c)
            {
              compute_cells_in_tree_recursively(forest,
                                                tree,
                                                tree_index,
                                                dealii_cell->child(c),
                                                p4est_child[c],
                                                u,
                                                cells_to_compute,
                                                computed_cells,
                                                new_needs);
            }
        }
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::exchange_data_on_cells(
      const std::vector<CellData> &cells_to_send,
      std::vector<CellData> &      received_cells) const
    {
      // First figure out where we need to send stuff. Some of the cells
      // in the input argument to this function might be destined for
      // the same process, so we have to only look at the unique set of
      // destinations:
      std::vector<types::subdomain_id> destinations;
      for (const auto &cell : cells_to_send)
        destinations.emplace_back(cell.receiver);
      std::sort(destinations.begin(), destinations.end());
      destinations.erase(std::unique(destinations.begin(), destinations.end()),
                         destinations.end());

      // Then set up the send/receive operation. This is best done through
      // the 'consensus algorithm' setup that is used for point-to-point
      // communication of information in cases where we do not know up
      // front which processes (and from how many processes) we have to
      // expect information from.
      const auto get_destinations = [&destinations]() { return destinations; };

      const auto create_request =
        [&cells_to_send](const types::subdomain_id other_rank,
                         std::vector<char> &       send_buffer) {
          // We have to create a single std::vector<char> that encodes the
          // information from all cells that are to be sent to 'other_rank'.
          // Each of the objects we want to send has a 'pack_data()'
          // function, so we can just create a vector of std::vector<char>
          // into which we put these strings, and then use the serialization
          // framework to get it all into one string.
          std::vector<std::vector<char>> array_of_strings;
          for (const auto &cell : cells_to_send)
            if (cell.receiver == other_rank)
              array_of_strings.emplace_back(cell.pack_data());

          send_buffer = Utilities::pack(array_of_strings, false);
        };

      const auto prepare_buffer_for_answer =
        [](const unsigned int /*other_rank*/, std::vector<char> &recv_buffer) {
          // Nothing to do here, we don't actually want to send an answer
          recv_buffer.clear();
        };

      const auto answer_request =
        [&received_cells](const unsigned int       other_rank,
                          const std::vector<char> &buffer_recv,
                          std::vector<char> &      request_buffer) {
          // We got a message from 'other_rank', so let us decode the
          // message in the same way as we have assembled it above
          const std::vector<std::vector<char>> array_of_strings =
            Utilities::unpack<std::vector<std::vector<char>>>(buffer_recv,
                                                              false);

          for (const auto &s : array_of_strings)
            {
              CellData cell_data;
              cell_data.unpack_data(s);
              cell_data.receiver = other_rank;

              received_cells.emplace_back(std::move(cell_data));
            }

          // Nothing left to do here, we don't actually need to provide an
          // answer:
          request_buffer.clear();
        };

      const auto read_answer = [](const unsigned int /*other_rank*/,
                                  const std::vector<char> &recv_buffer) {
        // We don't put anything into the answers, so nothing should
        // have been coming out at this end either:
        (void)recv_buffer;
        Assert(recv_buffer.size() == 0, ExcInternalError());
      };

      Utilities::MPI::ConsensusAlgorithms::AnonymousProcess<char, char>
        operations(get_destinations,
                   create_request,
                   answer_request,
                   prepare_buffer_for_answer,
                   read_answer);

      Utilities::MPI::ConsensusAlgorithms::NBX<char, char>(operations,
                                                           communicator)
        .run();
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::add_new_need(
      const typename dealii::internal::p4est::types<dim>::forest &  forest,
      const typename dealii::internal::p4est::types<dim>::locidx &  tree_index,
      const typename DoFHandler<dim, spacedim>::cell_iterator &     dealii_cell,
      const typename dealii::internal::p4est::types<dim>::quadrant &p4est_cell,
      std::vector<CellData> &                                       new_needs)
    {
      const FiniteElement<dim, spacedim> &fe =
        dealii_cell->get_dof_handler().get_fe();
      const unsigned int dofs_per_cell = fe.n_dofs_per_cell();

      CellData cell_data(dofs_per_cell);
      cell_data.quadrant   = p4est_cell;
      cell_data.tree_index = tree_index;
      cell_data.receiver =
        dealii::internal::p4est::functions<dim>::comm_find_owner(
          const_cast<typename dealii::internal::p4est::types<dim>::forest *>(
            &forest),
          tree_index,
          &p4est_cell,
          dealii_cell->level_subdomain_id());

      cell_data_insert(cell_data, new_needs);
    }



    template <int dim, int spacedim, class OutVector>
    int
    ExtrapolateImplementation<dim, spacedim, OutVector>::cell_data_search(
      const CellData &             cell_data,
      const std::vector<CellData> &cells_list)
    {
      typename std::vector<CellData>::const_iterator bound =
        std::lower_bound(cells_list.begin(), cells_list.end(), cell_data);

      if ((bound != cells_list.end()) && !(cell_data < *bound))
        return static_cast<int>(bound - cells_list.begin());

      return -1;
    }



    template <int dim, int spacedim, class OutVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::cell_data_insert(
      const CellData &       cell_data,
      std::vector<CellData> &cells_list)
    {
      typename std::vector<CellData>::iterator bound =
        std::lower_bound(cells_list.begin(), cells_list.end(), cell_data);

      if ((bound == cells_list.end()) || (cell_data < *bound))
        cells_list.insert(bound, 1, cell_data);
    }



    template <int dim, int spacedim, class OutVector>
    template <class InVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::
      compute_all_non_local_data(const DoFHandler<dim, spacedim> &dof2,
                                 const InVector &                 u)
    {
      std::vector<CellData> cells_we_need, cells_to_compute, received_cells,
        received_needs, new_needs, computed_cells, cells_to_send;

      // reset the round count we will use in send_cells
      round = 0;

      // Compute all the cells needed from other processes.
      compute_needs(dof2, cells_we_need);

      // Send the cells needed to their owners and receive
      // a list of cells other processes need from us.
      exchange_data_on_cells(cells_we_need, received_needs);

      // The list of received needs can contain some cells more than once
      // because different processes may need data from the same cell.
      // To compute data only once, generate a vector with unique entries
      // and distribute the computed data afterwards back to a vector with
      // correct receivers.
      // Computing cell_data can cause a need for data from some new cells.
      // If a cell is computed, send it back to their senders, maybe receive
      // new needs and compute again, do not wait that all cells are computed
      // or all needs are collected.
      // Otherwise we can run into a deadlock if a cell needed from another
      // process itself needs some data from us.
      unsigned int ready = 0;
      do
        {
          for (unsigned int i = 0; i < received_needs.size(); ++i)
            cell_data_insert(received_needs[i], cells_to_compute);

          compute_cells(dof2, u, cells_to_compute, computed_cells, new_needs);

          // if there are no cells to compute and no new needs, stop
          ready =
            Utilities::MPI::sum(new_needs.size() + cells_to_compute.size(),
                                communicator);

          for (const auto &comp : computed_cells)
            {
              // store computed cells...
              cell_data_insert(comp, available_cells);

              // ...and generate a vector of computed cells with correct
              // receivers, then delete this received need from the list
              typename std::vector<CellData>::iterator recv =
                received_needs.begin();
              while (recv != received_needs.end())
                {
                  if (dealii::internal::p4est::quadrant_is_equal<dim>(
                        recv->quadrant, comp.quadrant))
                    {
                      recv->dof_values = comp.dof_values;
                      cells_to_send.push_back(*recv);
                      received_needs.erase(recv);
                      recv = received_needs.begin();
                    }
                  else
                    ++recv;
                }
            }

          // increase the round counter, such that we are sure to only send
          // and receive data from the correct call
          ++round;

          exchange_data_on_cells(cells_to_send, received_cells);

          // store received cell_data
          for (const auto &recv : received_cells)
            {
              cell_data_insert(recv, available_cells);
            }

          // increase the round counter, such that we are sure to only send
          // and receive data from the correct call
          ++round;

          // finally send and receive new needs and start a new round
          exchange_data_on_cells(new_needs, received_needs);
        }
      while (ready != 0);
    }



    template <int dim, int spacedim, class OutVector>
    template <class InVector>
    void
    ExtrapolateImplementation<dim, spacedim, OutVector>::extrapolate_parallel(
      const InVector &                 u2_relevant,
      const DoFHandler<dim, spacedim> &dof2,
      OutVector &                      u2)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *tr =
        (dynamic_cast<const parallel::distributed::Triangulation<dim, spacedim>
                        *>(&dof2.get_triangulation()));

      Assert(
        tr != nullptr,
        ExcMessage(
          "Extrapolate in parallel only works for parallel distributed triangulations!"));

      communicator = tr->get_communicator();

      compute_all_non_local_data(dof2, u2_relevant);

      // exclude dofs on more refined ghosted cells
      const FiniteElement<dim, spacedim> &fe = dof2.get_fe();
      if (fe.max_dofs_per_face() > 0)
        {
          const unsigned int dofs_per_cell = fe.n_dofs_per_cell();
          std::vector<types::global_dof_index> indices(dofs_per_cell);

          for (const auto &cell : dof2.active_cell_iterators())
            if (cell->is_ghost())
              {
                cell->get_dof_indices(indices);
                for (const unsigned int face : cell->face_indices())
                  if (!cell->at_boundary(face))
                    {
                      const typename DoFHandler<dim, spacedim>::cell_iterator
                        neighbor = cell->neighbor(face);
                      if (neighbor->level() != cell->level())
                        for (unsigned int i = 0; i < fe.n_dofs_per_face(face);
                             ++i)
                          {
                            const types::global_dof_index index =
                              indices[fe.face_to_cell_index(i, face)];
                            ;
                            const bool index_stored =
                              (dofs_on_refined_neighbors.find(index) !=
                               dofs_on_refined_neighbors.end());
                            const unsigned int level =
                              index_stored ?
                                std::max(cell->level(),
                                         dofs_on_refined_neighbors[index]) :
                                cell->level();
                            dofs_on_refined_neighbors[index] = level;
                          }
                    }
              }
        }

      // after all dof values on
      // not owned patch cells
      // are computed, start
      // the interpolation
      u2 = typename OutVector::value_type(0.);

      std::queue<WorkPackage> queue;
      {
        for (const auto &cell : dof2.cell_iterators_on_level(0))
          {
            if (dealii::internal::p4est::tree_exists_locally<dim>(
                  tr->parallel_forest,
                  tr->coarse_cell_to_p4est_tree_permutation[cell->index()]) ==
                false)
              continue;

            typename dealii::internal::p4est::types<dim>::quadrant
              p4est_coarse_cell;
            const typename dealii::internal::p4est::types<dim>::locidx
              tree_index =
                tr->coarse_cell_to_p4est_tree_permutation[cell->index()];
            typename dealii::internal::p4est::types<dim>::tree *tree =
              tr->init_tree(cell->index());

            dealii::internal::p4est::init_coarse_quadrant<dim>(
              p4est_coarse_cell);

            queue.push({*tr->parallel_forest,
                        *tree,
                        tree_index,
                        cell,
                        p4est_coarse_cell});
          }
      }

      while (!queue.empty())
        {
          const WorkPackage &data = queue.front();

          const typename dealii::internal::p4est::types<dim>::forest &forest =
            data.forest;
          const typename dealii::internal::p4est::types<dim>::tree &tree =
            data.tree;
          const typename dealii::internal::p4est::types<dim>::locidx
            &tree_index = data.tree_index;
          const typename DoFHandler<dim, spacedim>::cell_iterator &dealii_cell =
            data.dealii_cell;
          const typename dealii::internal::p4est::types<dim>::quadrant
            &p4est_cell = data.p4est_cell;

          interpolate_recursively(
            forest, tree, tree_index, dealii_cell, p4est_cell, u2_relevant, u2);

          // traverse recursively over this tree
          if (dealii_cell->has_children())
            {
              Assert(dealii_cell->has_children(), ExcInternalError());
              typename dealii::internal::p4est::types<dim>::quadrant
                p4est_child[GeometryInfo<dim>::max_children_per_cell];

              dealii::internal::p4est::init_quadrant_children<dim>(p4est_cell,
                                                                   p4est_child);

              for (unsigned int c = 0;
                   c < GeometryInfo<dim>::max_children_per_cell;
                   ++c)
                queue.push({forest,
                            tree,
                            tree_index,
                            dealii_cell->child(c),
                            p4est_child[c]});
            }
          queue.pop();
        }


      u2.compress(VectorOperation::insert);
    }
#endif // DEAL_II_WITH_P4EST

    template <class VectorType, typename dummy = void>
    struct BlockTypeHelper
    {
      using type = VectorType;
    };

    template <class VectorType>
    struct BlockTypeHelper<
      VectorType,
      typename std::enable_if<IsBlockVector<VectorType>::value>::type>
    {
      using type = typename VectorType::BlockType;
    };

    template <class VectorType>
    using BlockType = typename BlockTypeHelper<VectorType>::type;

    template <class VectorType, class DH>
    void
    reinit_distributed(const DH &dh, VectorType &vector)
    {
      vector.reinit(dh.n_dofs());
    }

#ifdef DEAL_II_WITH_PETSC
    template <int dim, int spacedim>
    void
    reinit_distributed(const DoFHandler<dim, spacedim> &dh,
                       PETScWrappers::MPI::Vector &     vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());

      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      vector.reinit(locally_owned_dofs, parallel_tria->get_communicator());
    }
#endif // DEAL_II_WITH_PETSC

#ifdef DEAL_II_WITH_TRILINOS
    template <int dim, int spacedim>
    void
    reinit_distributed(const DoFHandler<dim, spacedim> &dh,
                       TrilinosWrappers::MPI::Vector &  vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());

      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      vector.reinit(locally_owned_dofs, parallel_tria->get_communicator());
    }



#  ifdef DEAL_II_WITH_MPI
#    ifdef DEAL_II_TRILINOS_WITH_TPETRA
    template <int dim, int spacedim, typename Number>
    void
    reinit_distributed(const DoFHandler<dim, spacedim> &              dh,
                       LinearAlgebra::TpetraWrappers::Vector<Number> &vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());

      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      vector.reinit(locally_owned_dofs, parallel_tria->get_communicator());
    }
#    endif

    template <int dim, int spacedim>
    void
    reinit_distributed(const DoFHandler<dim, spacedim> &      dh,
                       LinearAlgebra::EpetraWrappers::Vector &vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());

      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      vector.reinit(locally_owned_dofs, parallel_tria->get_communicator());
    }
#  endif
#endif // DEAL_II_WITH_TRILINOS

    template <int dim, int spacedim, typename Number>
    void
    reinit_distributed(const DoFHandler<dim, spacedim> &           dh,
                       LinearAlgebra::distributed::Vector<Number> &vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());

      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      vector.reinit(locally_owned_dofs, parallel_tria->get_communicator());
    }



    template <class VectorType, class DH>
    void
    reinit_ghosted(const DH & /*dh*/, VectorType & /*vector*/)
    {
      Assert(false, ExcNotImplemented());
    }

#ifdef DEAL_II_WITH_PETSC
    template <int dim, int spacedim>
    void
    reinit_ghosted(const DoFHandler<dim, spacedim> &dh,
                   PETScWrappers::MPI::Vector &     vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());
      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      IndexSet        locally_relevant_dofs;
      DoFTools::extract_locally_relevant_dofs(dh, locally_relevant_dofs);
      vector.reinit(locally_owned_dofs,
                    locally_relevant_dofs,
                    parallel_tria->get_communicator());
    }
#endif // DEAL_II_WITH_PETSC

#ifdef DEAL_II_WITH_TRILINOS
    template <int dim, int spacedim>
    void
    reinit_ghosted(const DoFHandler<dim, spacedim> &dh,
                   TrilinosWrappers::MPI::Vector &  vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());
      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      IndexSet        locally_relevant_dofs;
      DoFTools::extract_locally_relevant_dofs(dh, locally_relevant_dofs);
      vector.reinit(locally_owned_dofs,
                    locally_relevant_dofs,
                    parallel_tria->get_communicator());
    }
#endif // DEAL_II_WITH_TRILINOS

    template <int dim, int spacedim, typename Number>
    void
    reinit_ghosted(const DoFHandler<dim, spacedim> &           dh,
                   LinearAlgebra::distributed::Vector<Number> &vector)
    {
      const parallel::distributed::Triangulation<dim, spacedim> *parallel_tria =
        dynamic_cast<
          const parallel::distributed::Triangulation<dim, spacedim> *>(
          &dh.get_triangulation());
      Assert(parallel_tria != nullptr, ExcNotImplemented());
      const IndexSet &locally_owned_dofs = dh.locally_owned_dofs();
      IndexSet        locally_relevant_dofs;
      DoFTools::extract_locally_relevant_dofs(dh, locally_relevant_dofs);
      vector.reinit(locally_owned_dofs,
                    locally_relevant_dofs,
                    parallel_tria->get_communicator());
    }



    /**
     * Perform the extrapolation of a finite element field to patch-based
     * values for serial triangulations.
     */
    template <int dim, class InVector, class OutVector, int spacedim>
    void
    extrapolate_serial(const InVector &                 u3,
                       const DoFHandler<dim, spacedim> &dof2,
                       OutVector &                      u2)
    {
      const unsigned int dofs_per_cell = dof2.get_fe().n_dofs_per_cell();
      Vector<typename OutVector::value_type> dof_values(dofs_per_cell);

      // Then traverse grid bottom up, excluding the finest level
      for (unsigned int level = 0;
           level < dof2.get_triangulation().n_levels() - 1;
           ++level)
        {
          for (const auto &cell : dof2.cell_iterators_on_level(level))
            if (cell->has_children())
              {
                // Check whether this cell has active children
                bool active_children = false;
                for (unsigned int child_n = 0; child_n < cell->n_children();
                     ++child_n)
                  if (cell->child(child_n)->is_active())
                    {
                      active_children = true;
                      break;
                    }

                // If there are active children, this process
                // has to work on this cell. Get the values from the input
                // vector on this cell and interpolate it from the current
                // cell to its children in the other vector. If any of the
                // children have children themselves, then we will visit it
                // later in the outer loop here and overwrite the results in
                // u2.
                if (active_children)
                  {
                    cell->get_interpolated_dof_values(u3, dof_values);
                    cell->set_dof_values_by_interpolation(dof_values, u2);
                  }
              }
        }
    }
  } // namespace internal



  template <int dim, class InVector, class OutVector, int spacedim>
  void
  extrapolate(const DoFHandler<dim, spacedim> &dof1,
              const InVector &                 u1,
              const DoFHandler<dim, spacedim> &dof2,
              OutVector &                      u2)
  {
    // Forward to the other function using an empty set of
    // constraints.
    AffineConstraints<typename OutVector::value_type> dummy;
    dummy.close();
    extrapolate(dof1, u1, dof2, dummy, u2);
  }



  template <int dim, class InVector, class OutVector, int spacedim>
  void
  extrapolate(
    const DoFHandler<dim, spacedim> &                        dof1,
    const InVector &                                         u1,
    const DoFHandler<dim, spacedim> &                        dof2,
    const AffineConstraints<typename OutVector::value_type> &constraints,
    OutVector &                                              u2)
  {
    Assert(dof1.get_fe_collection().size() == 1, ExcNotImplemented());
    Assert(dof2.get_fe_collection().size() == 1, ExcNotImplemented());

    Assert(dof1.get_fe(0).n_components() == dof2.get_fe(0).n_components(),
           ExcDimensionMismatch(dof1.get_fe(0).n_components(),
                                dof2.get_fe(0).n_components()));
    Assert(&dof1.get_triangulation() == &dof2.get_triangulation(),
           ExcTriangulationMismatch());
    Assert(u1.size() == dof1.n_dofs(),
           ExcDimensionMismatch(u1.size(), dof1.n_dofs()));
    Assert(u2.size() == dof2.n_dofs(),
           ExcDimensionMismatch(u2.size(), dof2.n_dofs()));

    // make sure that each cell on the coarsest level is at least once refined,
    // otherwise, these cells can't be treated and would generate a bogus result
    for (const auto &cell : dof2.cell_iterators_on_level(0))
      {
        (void)cell;
        Assert(cell->has_children() || cell->is_artificial(),
               ExcGridNotRefinedAtLeastOnce());
      }


    internal::BlockType<OutVector> u3;
    internal::reinit_distributed(dof2, u3);
    if (dynamic_cast<const parallel::distributed::Triangulation<dim, spacedim>
                       *>(&dof2.get_triangulation()) != nullptr)
      {
        Assert(dof1.get_fe()[0].reference_cell() ==
                 ReferenceCells::get_hypercube<dim>(),
               ExcNotImplemented());
        Assert(dof2.get_fe()[0].reference_cell() ==
                 ReferenceCells::get_hypercube<dim>(),
               ExcNotImplemented());

        interpolate(dof1, u1, dof2, constraints, u3);

        internal::BlockType<OutVector> u3_relevant;
        internal::reinit_ghosted(dof2, u3_relevant);
        u3_relevant = u3;

        internal::ExtrapolateImplementation<dim, spacedim, OutVector>
          implementation;
        implementation.extrapolate_parallel(u3_relevant, dof2, u2);
      }
    else
      {
        interpolate(dof1, u1, dof2, constraints, u3);

        internal::extrapolate_serial(u3, dof2, u2);
      }

    constraints.distribute(u2);
  }
} // end of namespace FETools

DEAL_II_NAMESPACE_CLOSE

/*--------------------   fe_tools_extrapolate_templates.h -------------------*/
#endif // dealii_fe_tools_extrapolate_templates_H
