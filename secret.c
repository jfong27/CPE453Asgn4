#include <minix/drivers.h>
#include <minix/driver.h>
#include <stdio.h>
#include <stdlib.h>
#include <minix/ds.h>
#include <minix/ioctl.h>
#include <sys/ioc_secret.h>
#include "secret.h"

/*
 * Function prototypes for the secret driver.
 */
FORWARD _PROTOTYPE( char * secret_name, (void));
FORWARD _PROTOTYPE( int secret_open,      (struct driver *d, message *m) );
FORWARD _PROTOTYPE( int secret_close,     (struct driver *d, message *m) );
FORWARD _PROTOTYPE( int secret_ioctl, (struct driver *d, message *m));
FORWARD _PROTOTYPE( struct device * secret_prepare, (int device) );
FORWARD _PROTOTYPE( int secret_transfer,  (int procnr, int opcode,
                                          u64_t position, iovec_t *iov,
                                          unsigned nr_req) );
FORWARD _PROTOTYPE( void secret_geometry, (struct partition *entry) );

/* SEF functions and variables. */
FORWARD _PROTOTYPE( void sef_local_startup, (void) );
FORWARD _PROTOTYPE( int sef_cb_init, (int type, sef_init_info_t *info) );
FORWARD _PROTOTYPE( int sef_cb_lu_state_save, (int) );
FORWARD _PROTOTYPE( int lu_state_restore, (void) );

#define SECRET_SIZE 8192
#define O_WRONLY 2
#define O_RDONLY 4
#define O_RDWR 6
#define UNOWNED -1 

PRIVATE uid_t secretHolder = UNOWNED;
PRIVATE char secret[SECRET_SIZE];
PRIVATE int currWritePlace = 0, currReadPlace = 0; 
PRIVATE int lastWasRead = FALSE;

/* Entry points to the hello driver. */
PRIVATE struct driver secret_tab =
{
    secret_name,
    secret_open,
    secret_close,
    secret_ioctl,
    secret_prepare,
    secret_transfer,
    nop_cleanup,
    secret_geometry,
    nop_alarm,
    nop_cancel,
    nop_select,
    nop_ioctl,
    do_nop,
};

/** Represents the /dev/secret device. */
PRIVATE struct device secret_device;

PRIVATE int open_fds;

PRIVATE char * secret_name(void)
{
    return "secret";
}

PRIVATE int secret_open(
    struct driver *d, message *m) {
	struct ucred user;
	if((m->COUNT&0x07) == O_RDWR) {
		return EACCES;
	}

	getnucred(m->IO_ENDPT, &user);
    printf("%d\n", secretHolder);
	if(secretHolder == UNOWNED) {
		if((m->COUNT&0x07) == O_WRONLY) {
            open_fds++;
			secretHolder = user.uid;
			return OK;
		} else if((m->COUNT&0x07) == O_RDONLY) {
            lastWasRead = TRUE;
            open_fds++;
			secretHolder = user.uid;
			return OK;
		} else {
			return -1;
		}
	} else {
        if(secretHolder != user.uid)
        {
            return EACCES;
        }
		if((m->COUNT&0x07) == O_RDONLY) {
            lastWasRead = TRUE;
            open_fds++;
			return OK;	
		} else if((m->COUNT&0x07) == O_WRONLY){
			return ENOSPC;	
		}
	}
    open_fds++;
	return OK;
}

PRIVATE int secret_close(struct driver *d, message *m) {

   /* No more processes using secret, so clear it out */
   int i;
   open_fds -= 1;
   if (lastWasRead && open_fds == 0) {
      for (i = 0; i < SECRET_SIZE; i++) {
         secret[i] = '\0';
      }
      secretHolder = UNOWNED;
      lastWasRead = FALSE;
   }
   return OK;
}

PRIVATE int secret_ioctl(struct driver *d, message *m) {
   int res;
   uid_t grantee;

   if (m->REQUEST != SSGRANT) {
      return ENOTTY;
   }

   res = sys_safecopyfrom(m->IO_ENDPT, (vir_bytes)m->IO_GRANT,
                          0, (vir_bytes)&grantee, sizeof(grantee), D);

   secretHolder = grantee;

   return OK;
}

PRIVATE struct device * secret_prepare(dev)
    int dev;
{
    secret_device.dv_base.lo = 0;
    secret_device.dv_base.hi = 0;
    secret_device.dv_size.lo = strlen(secret);
    secret_device.dv_size.hi = 0;
    return &secret_device;
}

PRIVATE int secret_transfer(proc_nr, opcode, position, iov, nr_req)
    int proc_nr;
    int opcode;
    u64_t position;
    iovec_t *iov;
    unsigned nr_req;
{
    int readBytes, writeBytes, ret;
    switch (opcode)
    {
        case DEV_GATHER_S:
            readBytes = iov->iov_size;
            if(readBytes > (currWritePlace - currReadPlace))
            {
                readBytes = currWritePlace - currReadPlace;
            }
            if(readBytes <= 0) 
            {
                return OK;
            }
            ret = sys_safecopyto(proc_nr, iov->iov_addr, 0,
                                (vir_bytes) (secret + currReadPlace),
                                 readBytes, D);
            iov->iov_size -= readBytes;
            currReadPlace += readBytes;
            break;

	    case DEV_SCATTER_S:
            writeBytes = iov->iov_size;
            if(writeBytes > SECRET_SIZE-currWritePlace) {
                writeBytes = SECRET_SIZE-currWritePlace;
            }
            if(writeBytes <= 0) {
                return OK;
            }
	        ret = sys_safecopyfrom(proc_nr, iov->iov_addr, 0, 
                                  (vir_bytes) (secret + currWritePlace),
				                    writeBytes, D);
	        iov->iov_size += writeBytes;
            currWritePlace += writeBytes;
        default:
            return EINVAL;
    }
    return ret;
}

PRIVATE void secret_geometry(entry)
    struct partition *entry;
{
    entry->cylinders = 0;
    entry->heads     = 0;
    entry->sectors   = 0;
}

PRIVATE int sef_cb_lu_state_save(int state) {
/* Save the state. */
    ds_publish_mem("secret", secret, SECRET_SIZE, DSF_OVERWRITE);
    ds_publish_mem("secretHolder", &secretHolder, sizeof(secretHolder), DSF_OVERWRITE);
    ds_publish_mem("currWritePlace", &currWritePlace, sizeof(currWritePlace), DSF_OVERWRITE);
    ds_publish_mem("currReadPlace", &currReadPlace, sizeof(currReadPlace), DSF_OVERWRITE);
    ds_publish_mem("open_fds", &open_fds, sizeof(open_fds), DSF_OVERWRITE);
    ds_publish_mem("lastWasRead", &lastWasRead, sizeof(lastWasRead), DSF_OVERWRITE);

    return OK;
}

PRIVATE int lu_state_restore() {
/* Restore the state. */
    char * value;

    size_t *size;

    *size = SECRET_SIZE;
    ds_retrieve_mem("secret", value, size);
    ds_delete_mem("secret");
    strcpy(secret, value);

    *size = sizeof(secretHolder);
    ds_retrieve_mem("secretHolder", value, size);
    ds_delete_mem("secretHolder");
    secretHolder = (int) *value;

    *size = sizeof(currWritePlace);    
    ds_retrieve_mem("currWritePlace", value, size);
    ds_delete_mem("currWritePlace");
    currWritePlace = (int) value;

    *size = sizeof(currReadPlace);
    ds_retrieve_mem("currReadPlace", value, size);
    ds_delete_mem("currReadPlace");
    currReadPlace = (int) value;

    *size = sizeof(open_fds);
    ds_retrieve_mem("open_fds", value, size);
    ds_delete_mem("open_fds");
    open_fds = (int) value;

    *size = sizeof(lastWasRead);
    ds_retrieve_mem("lastWasRead", value, size);
    ds_delete_mem("lastWasRead");
    lastWasRead = (int) value;

    return OK;
}

PRIVATE void sef_local_startup()
{
    /*
     * Register init callbacks. Use the same function for all event types
     */
    sef_setcb_init_fresh(sef_cb_init);
    sef_setcb_init_lu(sef_cb_init);
    sef_setcb_init_restart(sef_cb_init);

    /*
     * Register live update callbacks.
     */
    /* - Agree to update immediately when LU is requested in a valid state. */
    sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
    /* - Support live update starting from any standard state. */
    sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_standard);
    /* - Register a custom routine to save the state. */
    sef_setcb_lu_state_save(sef_cb_lu_state_save);

    /* Let SEF perform startup. */
    sef_startup();
}

PRIVATE int sef_cb_init(int type, sef_init_info_t *info)
{
/* Initialize the hello driver. */
    int do_announce_driver = TRUE;

    switch(type) {
        case SEF_INIT_FRESH:
            printf("%s", secret);
        break;

        case SEF_INIT_LU:
            /* Restore the state. */
            lu_state_restore();
            do_announce_driver = FALSE;

            printf("%sHey, I'm a new version!\n", secret);
        break;

        case SEF_INIT_RESTART:
            printf("%sHey, I've just been restarted!\n", secret);
        break;
    }

    /* Announce we are up when necessary. */
    if (do_announce_driver) {
        driver_announce();
    }

    /* Initialization completed successfully. */
    return OK;
}

PUBLIC int main(int argc, char **argv)
{
    /*
     * Perform initialization.
     */
    sef_local_startup();
    /*
     * Run the main loop.
     */
    driver_task(&secret_tab, DRIVER_STD);
    return OK;
}

