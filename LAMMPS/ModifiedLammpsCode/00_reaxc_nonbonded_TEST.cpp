/*
 * 00_reaxc_nonbonded_TEST.cpp
 *
 *  Created on: Jun 19, 2019
 *      Author: mattcohe
 *      mostly taken from lammps with me and Ethan's modification for Accelerated ReaxFF
 */
/*----------------------------------------------------------------------
  PuReMD - Purdue ReaxFF Molecular Dynamics Program

  Copyright (2010) Purdue University
  Hasan Metin Aktulga, hmaktulga@lbl.gov
  Joseph Fogarty, jcfogart@mail.usf.edu
  Sagar Pandit, pandit@usf.edu
  Ananth Y Grama, ayg@cs.purdue.edu

  Please cite the related publication:
  H. M. Aktulga, J. C. Fogarty, S. A. Pandit, A. Y. Grama,
  "Parallel Reactive Molecular Dynamics: Numerical Methods and
  Algorithmic Techniques", Parallel Computing, in press.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details:
  <http://www.gnu.org/licenses/>.
  ----------------------------------------------------------------------*/

#include "pair_reaxc.h"
#include "reaxc_types.h"
#include "reaxc_nonbonded.h"
#include "reaxc_bond_orders.h"
#include "reaxc_list.h"
#include "reaxc_vector.h"
#include <iostream>
#include <fstream>


void vdW_Coulomb_Energy( reax_system *system, control_params *control,
                         simulation_data *data, storage *workspace,
                         reax_list **lists, output_controls * /*out_control*/ )
{
  int i, j, pj, natoms;
  int start_i, end_i, flag;
  rc_tagint orig_i, orig_j;
  double p_vdW1, p_vdW1i;
  double powr_vdW1, powgi_vdW1;
  double tmp, r_ij, fn13, exp1, exp2;
  double Tap, dTap, dfn13, CEvd, CEclmb, de_core;
  double dr3gamij_1, dr3gamij_3;
  double e_ele, e_vdW, e_core, SMALL = 0.0001;
  double e_lg, de_lg, r_ij5, r_ij6, re6;
  rvec temp, ext_press;
  two_body_parameters *twbp;
  far_neighbor_data *nbr_pj;
  reax_list *far_nbrs;

  // Tallying variables:
  double pe_vdw, f_tmp, delij[3];

  natoms = system->n;
  far_nbrs = (*lists) + FAR_NBRS;
  p_vdW1 = system->reax_param.gp.l[28];
  p_vdW1i = 1.0 / p_vdW1;
  e_core = 0;
  e_vdW = 0;
  e_lg = de_lg = 0.0;
  /*-------------------------------- read in restraint data --------------------------------*/
  double F1rest, F2rest, R12rest, Erest, dErestdr;
  int id1rest, id2rest;
  double Rijrest, dxrest, dyrest, dzrest, Rminrest, Rmaxrest, dErestx, dEresty, dErestz;
  int currStep = 0;
  int pastStep= 0;
  std::fstream doublefile;		//this is to prevent double application of force. really silly we have to use a file but my scope!
  //basically records past step to file, and if it equals current step we prevent it from running.
  //uses currStep, pastStep. see readfile.cpp for justification.
  int nRowsrest;
  double** restMatrix; // pointer to pointer (dynamic memory allocation); is deallocated at end of file
  std::ifstream testFile;		//this is where we initialize our test file, which we will be writing to
  testFile.open("nonbonded_TEST.txt", std::ios_base::app);

  //this fixes the issue where everytime we call run("10000") in python it runs this file twice.
  // read in the restraint parameter data file
  std::ifstream restfile ("rest-data.txt");
  if (data->step % 10000 == 0){			//only read file every 10000 steps, kind of silly to do it every timestep but who knows if it matters.
	  if (restfile.is_open()) {
	  	  restfile >> nRowsrest;

	  	  // allocate
	  	  restMatrix = new double*[nRowsrest];
	  	  for (i = 0; i < nRowsrest; i++) {
	  		  restMatrix[i] = new double[7];
	  	  }
	  	  // fill 2d array with infile parameters
	  	  for (i = 0; i < nRowsrest; i++) {
	  		  for (j = 0; j < 7; j++) {
	  			  restfile >> restMatrix[i][j];
	  		  }
	  	  }
	  	  if (data->step % 1000 == 0){
	  		//testFile<<"nRowsrest: "<<nRowsrest<<"\n";
	  		for (i = 0; i < nRowsrest; i++) {
	  	  		for (j = 0; j < 7; j++) {
	  	  			testFile << restMatrix[i][j] << "  ";
	  	  		}
	  	  		testFile << "\n";
	  	  	}
	  	  	   }

	  	  restfile.close();
	    }
	    else {
	      std::cout << "Unable to open restraint parameter file" << std::endl;
	  	restMatrix = new double*[1]; // wouldn't be used in this case anyway?
	    }
  }
  /*-------------------------------- end of restraint read --------------------------------*/
  currStep = data->step;
  doublefile.open("doublefile.txt",std::ios::in);
  doublefile >> pastStep;
  doublefile.close();
  if(currStep != pastStep){
  	  for (int restk = 0; restk < nRowsrest; restk++) {
  		  id1rest = restMatrix[restk][0];
  		  id2rest = restMatrix[restk][1];
  		  R12rest = restMatrix[restk][2];
  		  F1rest = restMatrix[restk][3];
  		  F2rest = restMatrix[restk][4];
  		  Rminrest = restMatrix[restk][5];
  		  Rmaxrest = restMatrix[restk][6];
  		  //testFile <<"\n\nCACLULATING RESTRAINT FORCE";
  		  //testFile << "id1rest: "<<id1rest<< " id2rest: "<<id2rest<< " R12rest: "<<R12rest<< " F1rest: "<<F1rest<< " F2rest: "<<F2rest<< " Rminrest: "<<Rminrest<< " Rmaxrest: "<<Rmaxrest<<"\n";
  		  for ( i = 0; i < (natoms-1); i++ ) {
  			  //testFile<<"id1 compared to: "<< system->my_atoms[i].orig_id<<" = "<<(id1rest == system->my_atoms[i].orig_id)<<"   id2 compared to "<<system->my_atoms[i].orig_id<<" = "<<(id2rest == system->my_atoms[i].orig_id)<<"\n";
  			  if ( (id1rest == system->my_atoms[i].orig_id) or (id2rest == system->my_atoms[i].orig_id) ) {
  				  for ( j = (i+1); j < natoms; j++) {
  					  //testFile<<"id1 compared to: "<< system->my_atoms[j].orig_id<<" = "<<(id1rest == system->my_atoms[j].orig_id)<<"   id2 compared to "<<system->my_atoms[j].orig_id<<" = "<<(id2rest == system->my_atoms[j].orig_id)<<"\n";
  					  if ( (id1rest == system->my_atoms[j].orig_id) or (id2rest == system->my_atoms[j].orig_id) ) {
  						  dxrest = system->my_atoms[i].x[0] - system->my_atoms[j].x[0];
  						  dyrest = system->my_atoms[i].x[1] - system->my_atoms[j].x[1];
  						  dzrest = system->my_atoms[i].x[2] - system->my_atoms[j].x[2];
  						  Rijrest = sqrt(dxrest*dxrest + dyrest*dyrest + dzrest*dzrest);
  						  //testFile<< "dxrest: "<<system->my_atoms[i].x[0]<<" - "<<system->my_atoms[j].x[0]<<" = "<<dxrest<<"\n";
  						  //testFile<< "dyrest: "<<system->my_atoms[i].x[1]<<" - "<<system->my_atoms[j].x[1]<<" = "<<dyrest<<"\n";
  						  //testFile<< "dzrest: "<<system->my_atoms[i].x[2]<<" - "<<system->my_atoms[j].x[2]<<" = "<<dzrest<<"\n";
  						  //testFile<< "Rijrest: "<<Rijrest<<"\n";
  						  //testFile<<"is Rijrest < Rmaxrest, and Rijrest > Rminrest "<<((Rijrest < Rmaxrest) and (Rijrest > Rminrest));
						  /*if ((((Rijrest < Rmaxrest) and (Rijrest > Rminrest)) != true) && data->step % 1000 == 0) {
							  testFile << id1rest << " and " << id2rest << " NOT Within restDistance: "<<Rijrest;
						  }*/
  						  //if ((Rijrest < Rmaxrest) and (Rijrest > Rminrest)) {
  							  Erest = F1rest*(1.0 - exp(-1.0*F2rest*(Rijrest - R12rest)*(Rijrest - R12rest) ));
  							  dErestdr = 2.0*F1rest*F2rest*(Rijrest - R12rest)*exp(-1.0*F2rest*(Rijrest - R12rest)*(Rijrest - R12rest));
  							  dErestx = dErestdr*dxrest/Rijrest;
  							  dEresty = dErestdr*dyrest/Rijrest;
  							  dErestz = dErestdr*dzrest/Rijrest;
  							  //this is to print every 20 timesteps 
							  //data->step is from pair_reax_c.cpp which reads from update->ntimestep
  							  if (data->step % 1000 == 0){
  								testFile<<"Timestep: "<<data->step<<"\n";
  								testFile<<"id1rest\natom ID: "<<system->my_atoms[i].orig_id<<" X: "<<system->my_atoms[i].x[0]<<" Y: "<<system->my_atoms[i].x[1]<<" Z: "<<system->my_atoms[i].x[2]<<"\n";
  								testFile<<"id2rest\natom ID: "<<system->my_atoms[j].orig_id<<" X: "<<system->my_atoms[j].x[0]<<" Y: "<<system->my_atoms[j].x[1]<<" Z: "<<system->my_atoms[j].x[2]<<"\n";
  								testFile<< "distance: "<<Rijrest<<"\n\n";
  							  }
  							  //testFile<< "Erest: "<<Erest<<"\n";
  							  //testFile<< "dErestdr: "<<dErestdr<<"\n";
  							  //testFile<< "dErestx: "<<dErestx<<"\n";
  							  //testFile<< "dEresty: "<<dEresty<<"\n";
  							  //testFile<< "dErestz: "<<dErestz<<"\n";
  							  temp[0] = dErestx;
  							  temp[1] = dEresty;
  							  temp[2] = dErestz;
  							  rvec_Add( workspace->f[i], temp);
  							  rvec_ScaledAdd( workspace->f[j], -1.0 ,temp);
  							  //testFile<<"dErestx, dEresty, dErestz added to workspace->f["<< i<<"]";
  						  //}
  					  }
  				  }
  			  }
  		  }
  	  }
  	doublefile.open("doublefile.txt",std::ios::out);
    doublefile << currStep ;
  	doublefile.close();
  	testFile.close();

  }
  /* End of added code for restraint force */
  for( i = 0; i < natoms; ++i ) {
    if (system->my_atoms[i].type < 0) continue;
    start_i = Start_Index(i, far_nbrs);
    end_i   = End_Index(i, far_nbrs);
    orig_i  = system->my_atoms[i].orig_id;

    for( pj = start_i; pj < end_i; ++pj ) {
      nbr_pj = &(far_nbrs->select.far_nbr_list[pj]);
      j = nbr_pj->nbr;
      if (system->my_atoms[j].type < 0) continue;
      orig_j  = system->my_atoms[j].orig_id;
      flag = 0;
      if(nbr_pj->d <= control->nonb_cut) {
        if (j < natoms) flag = 1;
        else if (orig_i < orig_j) flag = 1;
        else if (orig_i == orig_j) {
          if (nbr_pj->dvec[2] > SMALL) flag = 1;
          else if (fabs(nbr_pj->dvec[2]) < SMALL) {
            if (nbr_pj->dvec[1] > SMALL) flag = 1;
            else if (fabs(nbr_pj->dvec[1]) < SMALL && nbr_pj->dvec[0] > SMALL)
              flag = 1;
          }
        }
      }

      if (flag) {

      r_ij = nbr_pj->d;
      twbp = &(system->reax_param.tbp[ system->my_atoms[i].type ]
                                       [ system->my_atoms[j].type ]);

      Tap = workspace->Tap[7] * r_ij + workspace->Tap[6];
      Tap = Tap * r_ij + workspace->Tap[5];
      Tap = Tap * r_ij + workspace->Tap[4];
      Tap = Tap * r_ij + workspace->Tap[3];
      Tap = Tap * r_ij + workspace->Tap[2];
      Tap = Tap * r_ij + workspace->Tap[1];
      Tap = Tap * r_ij + workspace->Tap[0];

      dTap = 7*workspace->Tap[7] * r_ij + 6*workspace->Tap[6];
      dTap = dTap * r_ij + 5*workspace->Tap[5];
      dTap = dTap * r_ij + 4*workspace->Tap[4];
      dTap = dTap * r_ij + 3*workspace->Tap[3];
      dTap = dTap * r_ij + 2*workspace->Tap[2];
      dTap += workspace->Tap[1]/r_ij;

      /*vdWaals Calculations*/
      if(system->reax_param.gp.vdw_type==1 || system->reax_param.gp.vdw_type==3)
        { // shielding
          powr_vdW1 = pow(r_ij, p_vdW1);
          powgi_vdW1 = pow( 1.0 / twbp->gamma_w, p_vdW1);

          fn13 = pow( powr_vdW1 + powgi_vdW1, p_vdW1i );
          exp1 = exp( twbp->alpha * (1.0 - fn13 / twbp->r_vdW) );
          exp2 = exp( 0.5 * twbp->alpha * (1.0 - fn13 / twbp->r_vdW) );

          e_vdW = twbp->D * (exp1 - 2.0 * exp2);
          data->my_en.e_vdW += Tap * e_vdW;

          dfn13 = pow( powr_vdW1 + powgi_vdW1, p_vdW1i - 1.0) *
            pow(r_ij, p_vdW1 - 2.0);

          CEvd = dTap * e_vdW -
            Tap * twbp->D * (twbp->alpha / twbp->r_vdW) * (exp1 - exp2) * dfn13;
        }
      else{ // no shielding
        exp1 = exp( twbp->alpha * (1.0 - r_ij / twbp->r_vdW) );
        exp2 = exp( 0.5 * twbp->alpha * (1.0 - r_ij / twbp->r_vdW) );

        e_vdW = twbp->D * (exp1 - 2.0 * exp2);
        data->my_en.e_vdW += Tap * e_vdW;

        CEvd = dTap * e_vdW -
          Tap * twbp->D * (twbp->alpha / twbp->r_vdW) * (exp1 - exp2) / r_ij;
      }

      if(system->reax_param.gp.vdw_type==2 || system->reax_param.gp.vdw_type==3)
        { // inner wall
          e_core = twbp->ecore * exp(twbp->acore * (1.0-(r_ij/twbp->rcore)));
          data->my_en.e_vdW += Tap * e_core;

          de_core = -(twbp->acore/twbp->rcore) * e_core;
          CEvd += dTap * e_core + Tap * de_core / r_ij;

          //  lg correction, only if lgvdw is yes
          if (control->lgflag) {
            r_ij5 = pow( r_ij, 5.0 );
            r_ij6 = pow( r_ij, 6.0 );
            re6 = pow( twbp->lgre, 6.0 );
            e_lg = -(twbp->lgcij/( r_ij6 + re6 ));
            data->my_en.e_vdW += Tap * e_lg;

            de_lg = -6.0 * e_lg *  r_ij5 / ( r_ij6 + re6 ) ;
            CEvd += dTap * e_lg + Tap * de_lg / r_ij;
          }

        }

      /*Coulomb Calculations*/
      dr3gamij_1 = ( r_ij * r_ij * r_ij + twbp->gamma );
      dr3gamij_3 = pow( dr3gamij_1 , 0.33333333333333 );

      tmp = Tap / dr3gamij_3;
      data->my_en.e_ele += e_ele =
        C_ele * system->my_atoms[i].q * system->my_atoms[j].q * tmp;

      CEclmb = C_ele * system->my_atoms[i].q * system->my_atoms[j].q *
        ( dTap -  Tap * r_ij / dr3gamij_1 ) / dr3gamij_3;

      /* tally into per-atom energy */
      if( system->pair_ptr->evflag || system->pair_ptr->vflag_atom) {
        pe_vdw = Tap * (e_vdW + e_core + e_lg);
        rvec_ScaledSum( delij, 1., system->my_atoms[i].x,
                              -1., system->my_atoms[j].x );
        f_tmp = -(CEvd + CEclmb);
        system->pair_ptr->ev_tally(i,j,natoms,1,pe_vdw,e_ele,
                        f_tmp,delij[0],delij[1],delij[2]);
      }

      if( control->virial == 0 ) {
        rvec_ScaledAdd( workspace->f[i], -(CEvd + CEclmb), nbr_pj->dvec );
        rvec_ScaledAdd( workspace->f[j], +(CEvd + CEclmb), nbr_pj->dvec );
      }
      else { /* NPT, iNPT or sNPT */
        rvec_Scale( temp, CEvd + CEclmb, nbr_pj->dvec );

        rvec_ScaledAdd( workspace->f[i], -1., temp );
        rvec_Add( workspace->f[j], temp );

        rvec_iMultiply( ext_press, nbr_pj->rel_box, temp );
        rvec_Add( data->my_ext_press, ext_press );
      }
      }
    }
  }

  Compute_Polarization_Energy( system, data );
  // delete the dynamically allocated 2d array 'restMatrix'
  for (i = 0; i < nRowsrest; i++){
	  delete[] restMatrix[i];
  }
  delete[] restMatrix;
  // end deletion of dynamically allocated array
}



void Tabulated_vdW_Coulomb_Energy( reax_system *system,control_params *control,
                                   simulation_data *data, storage *workspace,
                                   reax_list **lists,
                                   output_controls * /*out_control*/ )
{
  int i, j, pj, r, natoms;
  int type_i, type_j, tmin, tmax;
  int start_i, end_i, flag;
  rc_tagint orig_i, orig_j;
  double r_ij, base, dif;
  double e_vdW, e_ele;
  double CEvd, CEclmb, SMALL = 0.0001;
  double f_tmp, delij[3];

  rvec temp, ext_press;
  far_neighbor_data *nbr_pj;
  reax_list *far_nbrs;
  LR_lookup_table *t;

  natoms = system->n;
  far_nbrs = (*lists) + FAR_NBRS;

  e_ele = e_vdW = 0;

  for( i = 0; i < natoms; ++i ) {
    type_i  = system->my_atoms[i].type;
    if (type_i < 0) continue;
    start_i = Start_Index(i,far_nbrs);
    end_i   = End_Index(i,far_nbrs);
    orig_i  = system->my_atoms[i].orig_id;

    for( pj = start_i; pj < end_i; ++pj ) {
      nbr_pj = &(far_nbrs->select.far_nbr_list[pj]);
      j = nbr_pj->nbr;
      type_j = system->my_atoms[j].type;
      if (type_j < 0) continue;
      orig_j  = system->my_atoms[j].orig_id;

      flag = 0;
      if(nbr_pj->d <= control->nonb_cut) {
        if (j < natoms) flag = 1;
        else if (orig_i < orig_j) flag = 1;
        else if (orig_i == orig_j) {
          if (nbr_pj->dvec[2] > SMALL) flag = 1;
          else if (fabs(nbr_pj->dvec[2]) < SMALL) {
            if (nbr_pj->dvec[1] > SMALL) flag = 1;
            else if (fabs(nbr_pj->dvec[1]) < SMALL && nbr_pj->dvec[0] > SMALL)
              flag = 1;
          }
        }
      }

      if (flag) {

      r_ij   = nbr_pj->d;
      tmin  = MIN( type_i, type_j );
      tmax  = MAX( type_i, type_j );
      t = &( LR[tmin][tmax] );

      /* Cubic Spline Interpolation */
      r = (int)(r_ij * t->inv_dx);
      if( r == 0 )  ++r;
      base = (double)(r+1) * t->dx;
      dif = r_ij - base;

      e_vdW = ((t->vdW[r].d*dif + t->vdW[r].c)*dif + t->vdW[r].b)*dif +
        t->vdW[r].a;

      e_ele = ((t->ele[r].d*dif + t->ele[r].c)*dif + t->ele[r].b)*dif +
        t->ele[r].a;
      e_ele *= system->my_atoms[i].q * system->my_atoms[j].q;

      data->my_en.e_vdW += e_vdW;
      data->my_en.e_ele += e_ele;

      CEvd = ((t->CEvd[r].d*dif + t->CEvd[r].c)*dif + t->CEvd[r].b)*dif +
        t->CEvd[r].a;

      CEclmb = ((t->CEclmb[r].d*dif+t->CEclmb[r].c)*dif+t->CEclmb[r].b)*dif +
        t->CEclmb[r].a;
      CEclmb *= system->my_atoms[i].q * system->my_atoms[j].q;

      /* tally into per-atom energy */
      if( system->pair_ptr->evflag || system->pair_ptr->vflag_atom) {
        rvec_ScaledSum( delij, 1., system->my_atoms[i].x,
                              -1., system->my_atoms[j].x );
        f_tmp = -(CEvd + CEclmb);
        system->pair_ptr->ev_tally(i,j,natoms,1,e_vdW,e_ele,
                        f_tmp,delij[0],delij[1],delij[2]);
      }

      if( control->virial == 0 ) {
        rvec_ScaledAdd( workspace->f[i], -(CEvd + CEclmb), nbr_pj->dvec );
        rvec_ScaledAdd( workspace->f[j], +(CEvd + CEclmb), nbr_pj->dvec );
      }
      else { // NPT, iNPT or sNPT
        rvec_Scale( temp, CEvd + CEclmb, nbr_pj->dvec );

        rvec_ScaledAdd( workspace->f[i], -1., temp );
        rvec_Add( workspace->f[j], temp );

        rvec_iMultiply( ext_press, nbr_pj->rel_box, temp );
        rvec_Add( data->my_ext_press, ext_press );
      }
      }
    }
  }

  Compute_Polarization_Energy( system, data );
}



void Compute_Polarization_Energy( reax_system *system, simulation_data *data )
{
  int  i, type_i;
  double q, en_tmp;

  data->my_en.e_pol = 0.0;
  for( i = 0; i < system->n; i++ ) {
    type_i = system->my_atoms[i].type;
    if (type_i < 0) continue;
    q = system->my_atoms[i].q;

    en_tmp = KCALpMOL_to_EV * (system->reax_param.sbp[type_i].chi * q +
                (system->reax_param.sbp[type_i].eta / 2.) * SQR(q));
    data->my_en.e_pol += en_tmp;

    /* tally into per-atom energy */
    if( system->pair_ptr->evflag)
      system->pair_ptr->ev_tally(i,i,system->n,1,0.0,en_tmp,0.0,0.0,0.0,0.0);
  }
}

void LR_vdW_Coulomb( reax_system *system, storage *workspace,
        control_params *control, int i, int j, double r_ij, LR_data *lr )
{
  double p_vdW1 = system->reax_param.gp.l[28];
  double p_vdW1i = 1.0 / p_vdW1;
  double powr_vdW1, powgi_vdW1;
  double tmp, fn13, exp1, exp2;
  double Tap, dTap, dfn13;
  double dr3gamij_1, dr3gamij_3;
  double e_core, de_core;
  double e_lg, de_lg, r_ij5, r_ij6, re6;
  two_body_parameters *twbp;

  twbp = &(system->reax_param.tbp[i][j]);
  e_core = 0;
  de_core = 0;
  e_lg = de_lg = 0.0;

  /* calculate taper and its derivative */
  Tap = workspace->Tap[7] * r_ij + workspace->Tap[6];
  Tap = Tap * r_ij + workspace->Tap[5];
  Tap = Tap * r_ij + workspace->Tap[4];
  Tap = Tap * r_ij + workspace->Tap[3];
  Tap = Tap * r_ij + workspace->Tap[2];
  Tap = Tap * r_ij + workspace->Tap[1];
  Tap = Tap * r_ij + workspace->Tap[0];

  dTap = 7*workspace->Tap[7] * r_ij + 6*workspace->Tap[6];
  dTap = dTap * r_ij + 5*workspace->Tap[5];
  dTap = dTap * r_ij + 4*workspace->Tap[4];
  dTap = dTap * r_ij + 3*workspace->Tap[3];
  dTap = dTap * r_ij + 2*workspace->Tap[2];
  dTap += workspace->Tap[1]/r_ij;

  /*vdWaals Calculations*/
  if(system->reax_param.gp.vdw_type==1 || system->reax_param.gp.vdw_type==3)
    { // shielding
      powr_vdW1 = pow(r_ij, p_vdW1);
      powgi_vdW1 = pow( 1.0 / twbp->gamma_w, p_vdW1);

      fn13 = pow( powr_vdW1 + powgi_vdW1, p_vdW1i );
      exp1 = exp( twbp->alpha * (1.0 - fn13 / twbp->r_vdW) );
      exp2 = exp( 0.5 * twbp->alpha * (1.0 - fn13 / twbp->r_vdW) );

      lr->e_vdW = Tap * twbp->D * (exp1 - 2.0 * exp2);

      dfn13 = pow( powr_vdW1 + powgi_vdW1, p_vdW1i-1.0) * pow(r_ij, p_vdW1-2.0);

      lr->CEvd = dTap * twbp->D * (exp1 - 2.0 * exp2) -
        Tap * twbp->D * (twbp->alpha / twbp->r_vdW) * (exp1 - exp2) * dfn13;
    }
  else{ // no shielding
    exp1 = exp( twbp->alpha * (1.0 - r_ij / twbp->r_vdW) );
    exp2 = exp( 0.5 * twbp->alpha * (1.0 - r_ij / twbp->r_vdW) );

    lr->e_vdW = Tap * twbp->D * (exp1 - 2.0 * exp2);
    lr->CEvd = dTap * twbp->D * (exp1 - 2.0 * exp2) -
      Tap * twbp->D * (twbp->alpha / twbp->r_vdW) * (exp1 - exp2) / r_ij;
  }

  if(system->reax_param.gp.vdw_type==2 || system->reax_param.gp.vdw_type==3)
    { // inner wall
      e_core = twbp->ecore * exp(twbp->acore * (1.0-(r_ij/twbp->rcore)));
      lr->e_vdW += Tap * e_core;

      de_core = -(twbp->acore/twbp->rcore) * e_core;
      lr->CEvd += dTap * e_core + Tap * de_core / r_ij;

      //  lg correction, only if lgvdw is yes
      if (control->lgflag) {
        r_ij5 = pow( r_ij, 5.0 );
        r_ij6 = pow( r_ij, 6.0 );
        re6 = pow( twbp->lgre, 6.0 );
        e_lg = -(twbp->lgcij/( r_ij6 + re6 ));
        lr->e_vdW += Tap * e_lg;

        de_lg = -6.0 * e_lg *  r_ij5 / ( r_ij6 + re6 ) ;
        lr->CEvd += dTap * e_lg + Tap * de_lg/r_ij;
      }

    }


  /* Coulomb calculations */
  dr3gamij_1 = ( r_ij * r_ij * r_ij + twbp->gamma );
  dr3gamij_3 = pow( dr3gamij_1 , 0.33333333333333 );

  tmp = Tap / dr3gamij_3;
  lr->H = EV_to_KCALpMOL * tmp;
  lr->e_ele = C_ele * tmp;

  lr->CEclmb = C_ele * ( dTap -  Tap * r_ij / dr3gamij_1 ) / dr3gamij_3;
}

