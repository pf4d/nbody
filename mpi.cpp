#include <mpi.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "common.h"

//
//  benchmarking program
//
int main( int argc, char **argv )
{    
  int    rnavg, navg, nabsavg=0;
  double rdavg, rdmin, dmin, davg, absmin=1.0, absavg=0.0;

  //
  //  process command line parameters
  //
  if( find_option( argc, argv, "-h" ) >= 0 )
  {
    printf( "Options:\n" );
    printf( "-h to see this help\n" );
    printf( "-n <int> to set the number of particles\n" );
    printf( "-o <filename> to specify the output file name\n" );
    printf( "-s <filename> to specify a summary file name\n" );
    printf( "-no turns off all correctness checks and particle output\n");
    return 0;
  }
  
  int n          = read_int( argc, argv, "-n", 1000 );
  char *savename = read_string( argc, argv, "-o", NULL );
  char *sumname  = read_string( argc, argv, "-s", NULL );
  
  //
  //  set up MPI
  //
  int n_proc, rank;
  MPI_Init( &argc, &argv );
  MPI_Comm_size( MPI_COMM_WORLD, &n_proc );
  MPI_Comm_rank( MPI_COMM_WORLD, &rank );
  
  //
  //  allocate generic resources
  //
  FILE *fsave = savename && rank == 0 ? fopen( savename, "w" ) : NULL;
  FILE *fsum  = sumname && rank  == 0 ? fopen ( sumname, "a" ) : NULL;

  particle_t *particles = (particle_t*) malloc( n * sizeof(particle_t) );
  
  MPI_Datatype PARTICLE;
  MPI_Type_contiguous( 6, MPI_DOUBLE, &PARTICLE );
  MPI_Type_commit( &PARTICLE );
  
  //
  //  set up the data partitioning across processors
  //
  int particle_per_proc  = (n + n_proc - 1) / n_proc;
  int *partition_offsets = (int*) malloc( (n_proc+1) * sizeof(int) );
  for( int i = 0; i < n_proc+1; i++ )
  {
    partition_offsets[i] = min( i * particle_per_proc, n );
  }
  
  int *partition_sizes = (int*) malloc( n_proc * sizeof(int) );
  for( int i = 0; i < n_proc; i++ )
  {
    partition_sizes[i] = partition_offsets[i+1] - partition_offsets[i];
  }
  
  //
  //  allocate storage for local partition
  //
  int nlocal        = partition_sizes[rank];
  particle_t* local = (particle_t*) malloc( nlocal * sizeof(particle_t) );
  particle_t* a_p   = NULL;
  
  //
  //  initialize and distribute the particles (that's fine to leave 
  //  it unoptimized)
  //
  double width = set_size( n );
  QuadTreeNode* root;
  if( rank == 0 )
  {
    a_p = (particle_t*) malloc( n_proc*n * sizeof(particle_t) );
    init_particles( n, particles );
  }
  MPI_Scatterv( particles, partition_sizes, partition_offsets, PARTICLE, 
                local, nlocal, PARTICLE, 0, MPI_COMM_WORLD );
  
  //
  //  simulate a number of time steps
  //
  double simulation_time = read_timer();
  for( int step = 0; step < NSTEPS; step++ )
  {
    navg = 0;
    dmin = 1.0;
    davg = 0.0;

    //
    //  collect all global data locally, from each processor's 
    //    'local' array to 'particles' array :
    //
    MPI_Allgatherv( local, nlocal, PARTICLE, particles, partition_sizes, 
                    partition_offsets, PARTICLE, MPI_COMM_WORLD);
    //MPI_Bcast( particles, n, PARTICLE, 0, MPI_COMM_WORLD );
    
    //
    //  save current step if necessary (slightly different semantics than in 
    //  other codes)
    //
    if( find_option( argc, argv, "-no" ) == -1 )
    {
      if( fsave && (step%SAVEFREQ) == 0 )
      {
        save( fsave, n, particles );
      }
    }
    
    //
    // initialze particles for this processor's quadtree :
    //
    root    = new QuadTreeNode(NULL, 0.0, 0.0, width, width, 1.0);
    root->init_particles( local, nlocal );
    root->computeCOM();
    
    //
    //  compute all forces
    //
    for( int i = 0; i < n; i++ )
    {
      particles[i].ax = particles[i].ay = 0;
      root->computeF( &particles[i], &dmin, &davg, &navg );
    }
    
    //
    //  Gather all the particles by rank 0 and update forces, then
    //    scatter the particles back to each proc's local array :
    //
    MPI_Gather( particles, n, PARTICLE, a_p, n, PARTICLE, 0, MPI_COMM_WORLD);
    if( rank == 0 )
    {
      for( int i = 0; i < n; i++)
      {
        for( int j = 0; j < n_proc; j++)
        {
          particles[i].ax += a_p[i + n*j].ax;
          particles[i].ay += a_p[i + n*j].ay;
        }
      }
    }
    MPI_Scatterv( particles, partition_sizes, partition_offsets, PARTICLE, 
                  local, nlocal, PARTICLE, 0, MPI_COMM_WORLD );
  
    if( find_option( argc, argv, "-no" ) == -1 )
    {
      MPI_Reduce(&davg,&rdavg,1,MPI_DOUBLE,MPI_SUM,0,MPI_COMM_WORLD);
      MPI_Reduce(&navg,&rnavg,1,MPI_INT,   MPI_SUM,0,MPI_COMM_WORLD);
      MPI_Reduce(&dmin,&rdmin,1,MPI_DOUBLE,MPI_MIN,0,MPI_COMM_WORLD);

      if (rank == 0)
      {
        //
        // Computing statistical data
        //
        if (rnavg)
        {
          absavg +=  rdavg/rnavg;
          nabsavg++;
        }
        if (rdmin < absmin)
        {
          absmin = rdmin;
        }
      }
    }

    //
    //  move particles, then delete the quadtree :
    //
    for( int i = 0; i < nlocal; i++)
    {
      move( local[i] );
    }
    delete root;
  }
  simulation_time = read_timer( ) - simulation_time;

  if (rank == 0)
  {  
    printf( "n = %d, simulation time = %g seconds", n, simulation_time);

    if( find_option( argc, argv, "-no" ) == -1 )
    {
      if (nabsavg)
      {
        absavg /= nabsavg;
      }
      // 
      //  -the minimum distance absmin between 2 particles during the run of 
      //   the simulation
      //  -A Correct simulation will have particles stay at greater than 0.4 
      //   (of cutoff) with typical values between .7-.8
      //  -A simulation were particles don't interact correctly will be less 
      //   than 0.4 (of cutoff) with typical values between .01-.05
      //
      //  -The average distance absavg is ~.95 when most particles are 
      //   interacting correctly and ~.66 when no particles are interacting
      //
      printf( ", absmin = %lf, absavg = %lf", absmin, absavg);
      if (absmin < 0.4)
      {
        printf ("\nThe minimum distance is below 0.4 meaning that some ");
        printf ("particle is not interacting");
      }
      if (absavg < 0.8)
      {
        printf ("\nThe average distance is below 0.8 meaning that most ");
        printf ("particles are not interacting");
      }
    }
    printf("\n");     
      
    //  
    // Printing summary data
    //  
    if( fsum)
    {
      fprintf(fsum,"%d %d %g\n",n,n_proc,simulation_time);
    }
  }

  //
  //  release resources
  //
  if ( fsum )
  {
    fclose( fsum );
  }
  
  free( partition_offsets );
  free( partition_sizes );
  free( local );
  free( particles );
  
  if( fsave )
  {
    fclose( fsave );
  }
  
  MPI_Finalize();
  return 0;
}



